// SPDX-License-Identifier: MIT
#include <cy/networking/world_replication.h>

#include <cy/ecs/query.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace cy::net {
namespace {

// The snapshot format's own costs, as `replication.cpp` writes them. Restated here because the
// replicator must know an entity's exact size before it decides to write it, and a budget checked
// against an estimate is a budget that is sometimes exceeded.
constexpr u32 kTagBits = 2;
constexpr u32 kIdBits = 64;
constexpr u32 kSchemaBits = 8;
constexpr u32 kSpawnBits = kTagBits + kIdBits + 32 + 32 + 32;
constexpr u32 kDespawnBits = kTagBits + kIdBits;
constexpr u32 kUpdateOverheadBits = kTagBits + kIdBits + kSchemaBits;
constexpr u32 kHeaderBits = 32 + 64 + 1 + kTagBits;

template <class T, class... Args>
[[nodiscard]] T* make(Allocator& allocator, Args&&... args) noexcept {
    void* storage = allocator.allocate(sizeof(T), alignof(T));
    if (storage == nullptr) {
        return nullptr;
    }
    return construct_at<T>(storage, std::forward<Args>(args)...);
}

template <class T>
void unmake(Allocator& allocator, T* object) noexcept {
    if (object != nullptr) {
        object->~T();
        allocator.deallocate(object, sizeof(T), alignof(T));
    }
}

[[nodiscard]] ecs::Entity unpack(u64 bits) noexcept {
    return ecs::Entity::from_bits(bits);
}

}  // namespace

// --- Registration --------------------------------------------------------------------------------

Expected<ecs::ComponentTypeId, Error> network_identity_component(ecs::World& world) noexcept {
    if (const ecs::ComponentInfo* found = world.components().find(kNetworkIdentityComponent);
        found != nullptr) {
        return found->id;
    }
    return world.components().register_builtin(kNetworkIdentityComponent,
                                               static_cast<u32>(sizeof(NetworkIdentity)),
                                               static_cast<u32>(alignof(NetworkIdentity)));
}

Status bind_schemas(const ecs::World& world, const SchemaSet& schemas,
                    Array<ecs::ComponentTypeId>& out) noexcept {
    out.clear();
    for (u32 index = 0; index < schemas.size(); ++index) {
        const ecs::ComponentInfo* info = world.components().find(schemas.at(index).type());
        if (info == nullptr) {
            return fail(ErrorCode::NotFound,
                        "networking: a replicated schema's type is not a component of this world");
        }
        if (Status pushed = out.push_back(info->id); !pushed) {
            return pushed;
        }
    }
    return ok();
}

// --- Schemas from reflection ---------------------------------------------------------------------

namespace {

[[nodiscard]] bool same(const char* a, const char* b) noexcept {
    return std::strcmp(a, b) == 0;
}

/// `bits=N` anywhere in the parameter text, or `fallback`.
[[nodiscard]] u32 bits_parameter(const char* parameters, u32 fallback) noexcept {
    const char* found = std::strstr(parameters, "bits=");
    if (found == nullptr) {
        return fallback;
    }
    const long value = std::strtol(found + 5, nullptr, 10);
    return value > 0 ? static_cast<u32>(value) : fallback;
}

[[nodiscard]] bool parse_encoder(const char* name, u32 field_size, FieldEncoder& out) noexcept {
    if (same(name, "raw") || name[0] == '\0') {
        out = FieldEncoder::Raw;
    } else if (same(name, "quantised")) {
        // A three-component field quantised component-wise is a vector; one is a scalar.
        out = field_size == 12 ? FieldEncoder::QuantisedVector : FieldEncoder::QuantisedScalar;
    } else if (same(name, "quantised-vector")) {
        out = FieldEncoder::QuantisedVector;
    } else if (same(name, "quaternion")) {
        out = FieldEncoder::CompressedQuaternion;
    } else if (same(name, "bitfield")) {
        out = FieldEncoder::Bitfield;
    } else if (same(name, "dictionary")) {
        out = FieldEncoder::DictionaryIndex;
    } else {
        return false;
    }
    return true;
}

[[nodiscard]] bool parse_condition(const char* name, FieldSchema& out) noexcept {
    out.condition = SendCondition::OnChange;
    out.filter = TargetFilter::Everyone;
    if (name[0] == '\0' || same(name, "on-change") || same(name, "owner-and-observers")) {
        return true;
    }
    if (same(name, "always")) {
        out.condition = SendCondition::Always;
    } else if (same(name, "owner-only")) {
        out.filter = TargetFilter::OwnerOnly;
    } else if (same(name, "except-owner")) {
        out.filter = TargetFilter::ExceptOwner;
    } else {
        return false;
    }
    return true;
}

[[nodiscard]] SchemaRejection rejection(const reflect::TypeInfo& type,
                                        const reflect::FieldInfo& field, SchemaError error,
                                        const char* detail) noexcept {
    SchemaRejection out;
    out.error = error;
    out.component = type.name;
    out.field = field.name;
    out.field_id = field.id;
    out.detail = detail;
    return out;
}

[[nodiscard]] Expected<FieldSchema, SchemaRejection> field_from_reflection(
    const reflect::TypeInfo& type, const reflect::FieldInfo& field) noexcept {
    const reflect::ReplicatedAttribute& declared = field.attributes.replicated;
    FieldSchema schema;
    schema.field = field.id;
    if (!parse_encoder(declared.encoder, field.size, schema.encoder)) {
        return make_unexpected(rejection(type, field, SchemaError::EncoderDoesNotApply,
                                         "an encoder name replication does not know"));
    }
    if (!parse_condition(declared.condition, schema)) {
        return make_unexpected(rejection(type, field, SchemaError::EncoderDoesNotApply,
                                         "a send condition replication does not know"));
    }
    const bool quantised = schema.encoder == FieldEncoder::QuantisedScalar ||
                           schema.encoder == FieldEncoder::QuantisedVector;
    if (quantised) {
        if (!field.attributes.declares(reflect::AttributeKind::Range)) {
            return make_unexpected(rejection(type, field, SchemaError::RangeCannotRepresent,
                                             "a quantised field needs a declared Range"));
        }
        schema.parameters.minimum = field.attributes.range.minimum;
        schema.parameters.maximum = field.attributes.range.maximum;
        schema.parameters.bits = bits_parameter(declared.parameters, 16);
    } else {
        schema.parameters.bits = bits_parameter(declared.parameters, field.size * 8);
    }
    return schema;
}

}  // namespace

Expected<ReplicationSchema, SchemaRejection> schema_from_reflection(
    const reflect::TypeInfo& type, Array<FieldSchema>& fields) noexcept {
    fields.clear();
    for (u32 index = 0; index < type.field_count; ++index) {
        const reflect::FieldInfo& field = type.fields[index];
        if (field.attributes.transient() ||
            !field.attributes.declares(reflect::AttributeKind::Replicated)) {
            continue;
        }
        Expected<FieldSchema, SchemaRejection> derived = field_from_reflection(type, field);
        if (!derived) {
            return make_unexpected(derived.error());
        }
        if (!fields.push_back(*derived)) {
            return make_unexpected(rejection(type, field, SchemaError::TooManyFields,
                                             "out of memory deriving the schema"));
        }
    }
    ReplicationSchema schema;
    schema.component = type.name;
    schema.version = 1;
    schema.fields = fields.span();
    return schema;
}

// --- WorldReplicator -----------------------------------------------------------------------------

/// One schema's column in one chunk, as `gather()` found it.
struct WorldReplicator::Column {
    const u8* base = nullptr;
    u64 version = 0;
};

/// One replicated entity, as `gather()` found it.
struct WorldReplicator::Record {
    NetworkId id;
    ecs::Entity entity;
    PeerId owner;
    u32 prefab = 0;
    /// The first of this entity's chunk's columns in `columns_`, one per schema.
    u32 columns = 0;
    u32 row = 0;
};

/// What `classify()` decided needs sending to a peer.
struct WorldReplicator::Dirty {
    u32 record = 0;
    bool spawn = false;
    /// The first of this entity's change masks in the peer's mask pool, one per schema.
    u32 masks = 0;
};

namespace {

/// A peer's knowledge of one entity.
struct EntitySync {
    enum class State : u8 {
        /// Spawned in some snapshot, none of which has been acknowledged. Repeated until one is.
        Spawning = 0,
        Live,
        /// Gone or no longer relevant; the despawn is repeated until a snapshot carrying it is
        /// acknowledged.
        Despawning,
    };

    State state = State::Spawning;
    /// The world version at which this peer's acknowledged baseline was taken. A chunk older than
    /// it holds nothing the peer lacks.
    u64 synced_version = 0;
    SnapshotId pending_snapshot = 0;
    u64 pending_version = 0;
};

}  // namespace

/// What one snapshot carried, so its acknowledgement can confirm exactly that.
struct WorldReplicator::SnapshotRecord {
    SnapshotId id = 0;
    u64 version = 0;
    bool acknowledged = false;
    Array<u64> sent;
    Array<u64> spawned;
    Array<u64> despawned;

    explicit SnapshotRecord(Allocator& allocator) noexcept
        : sent(allocator), spawned(allocator), despawned(allocator) {}
};

struct WorldReplicator::Peer {
    PeerId peer;
    PeerBaseline baseline;
    HashMap<u64, EntitySync> sync;
    Array<SnapshotRecord*> history;
    SnapshotId next = 1;
    PeerInterest interest{};
    bool has_interest = false;
    Array<Dirty> dirty;
    HashMap<u64, u32> dirty_index;
    Array<ChangeMask> masks;
    Array<Candidate> candidates;
    Array<u64> scratch;
    ReplicationReport report{};

    Peer(Allocator& allocator, const SchemaSet& schemas, PeerId id) noexcept
        : peer(id),
          baseline(allocator, schemas),
          sync(allocator),
          history(allocator),
          dirty(allocator),
          dirty_index(allocator),
          masks(allocator),
          candidates(allocator),
          scratch(allocator) {}
};

WorldReplicator::WorldReplicator(Allocator& allocator, ecs::World& world, const SchemaSet& schemas,
                                 const ReplicatorConfig& config) noexcept
    : allocator_(&allocator),
      world_(&world),
      schemas_(&schemas),
      config_(config),
      components_(allocator),
      columns_(allocator),
      records_(allocator),
      by_id_(allocator),
      peers_(allocator),
      interest_(allocator),
      scheduler_(allocator),
      relevant_(allocator),
      placed_(allocator),
      left_(allocator),
      selected_(allocator),
      writer_(allocator, schemas),
      out_(allocator) {
    scheduler_.set_weights(config_.weights);
    scheduler_.set_bands(config_.bands);
    scheduler_.set_distance_bands(config_.near_distance_squared, config_.far_distance_squared);
}

WorldReplicator::~WorldReplicator() {
    for (Peer* peer : peers_) {
        for (SnapshotRecord* record : peer->history) {
            unmake(*allocator_, record);
        }
        unmake(*allocator_, peer);
    }
}

Status WorldReplicator::initialize() noexcept {
    Expected<ecs::ComponentTypeId, Error> identity = network_identity_component(*world_);
    if (!identity) {
        return make_unexpected(identity.error());
    }
    identity_ = *identity;
    if (Status bound = bind_schemas(*world_, *schemas_, components_); !bound) {
        return bound;
    }
    initialized_ = true;
    return ok();
}

Expected<NetworkId, Error> WorldReplicator::replicate(ecs::Entity entity, u32 prefab,
                                                      PeerId owner) noexcept {
    if (!initialized_) {
        return fail(ErrorCode::Unavailable, "networking: the replicator is not initialised");
    }
    if (!world_->is_alive(entity)) {
        return fail(ErrorCode::NotFound, "networking: cannot replicate a dead entity");
    }
    if (world_->has(entity, identity_)) {
        return fail(ErrorCode::AlreadyExists, "networking: this entity is already replicated");
    }
    const NetworkId id = minter_.mint();
    NetworkIdentity identity;
    identity.id = id.value();
    identity.prefab = prefab;
    identity.owner_slot = owner.slot();
    identity.owner_generation = owner.generation();
    if (Status added = world_->add(entity, identity_, &identity); !added) {
        return make_unexpected(added.error());
    }
    return id;
}

WorldReplicator::Peer* WorldReplicator::find(PeerId peer) noexcept {
    for (Peer* candidate : peers_) {
        if (candidate->peer == peer) {
            return candidate;
        }
    }
    return nullptr;
}

const WorldReplicator::Peer* WorldReplicator::find(PeerId peer) const noexcept {
    for (const Peer* candidate : peers_) {
        if (candidate->peer == peer) {
            return candidate;
        }
    }
    return nullptr;
}

bool WorldReplicator::has_peer(PeerId peer) const noexcept {
    return find(peer) != nullptr;
}

Status WorldReplicator::add_peer(PeerId peer) noexcept {
    if (find(peer) != nullptr) {
        return fail(ErrorCode::AlreadyExists, "networking: that peer is already replicated to");
    }
    Peer* created = make<Peer>(*allocator_, *allocator_, *schemas_, peer);
    if (created == nullptr) {
        return fail(ErrorCode::OutOfMemory, "networking: could not allocate a peer's state");
    }
    for (u32 index = 0; index < kSnapshotHistory; ++index) {
        auto* record = make<SnapshotRecord>(*allocator_, *allocator_);
        if (record == nullptr || !created->history.push_back(record)) {
            unmake(*allocator_, record);
            for (SnapshotRecord* made : created->history) {
                unmake(*allocator_, made);
            }
            unmake(*allocator_, created);
            return fail(ErrorCode::OutOfMemory, "networking: could not allocate snapshot history");
        }
    }
    if (Status pushed = peers_.push_back(created); !pushed) {
        for (SnapshotRecord* made : created->history) {
            unmake(*allocator_, made);
        }
        unmake(*allocator_, created);
        return pushed;
    }
    return ok();
}

void WorldReplicator::remove_peer(PeerId peer) noexcept {
    for (usize index = 0; index < peers_.size(); ++index) {
        if (peers_[index]->peer == peer) {
            for (SnapshotRecord* record : peers_[index]->history) {
                unmake(*allocator_, record);
            }
            unmake(*allocator_, peers_[index]);
            peers_.remove_unordered(index);
            return;
        }
    }
}

Status WorldReplicator::set_interest(PeerId peer, const PeerInterest& interest,
                                     Span<const CellId> cells) noexcept {
    Peer* state = find(peer);
    if (state == nullptr) {
        return fail(ErrorCode::NotFound, "networking: no such peer");
    }
    state->interest = interest;
    state->interest.peer = peer;
    state->has_interest = true;
    return interest_.set_interest_cells(peer, cells);
}

ecs::Entity WorldReplicator::entity_of(NetworkId id) const noexcept {
    const u32* index = by_id_.find(id.value());
    return index == nullptr ? ecs::Entity{} : records_[*index].entity;
}

// --- Gather --------------------------------------------------------------------------------------

Status WorldReplicator::gather_chunk(void* query_chunk) noexcept {
    auto& chunk = *static_cast<ecs::QueryChunk*>(query_chunk);
    const u32 first = static_cast<u32>(columns_.size());
    ChunkView view = chunk.archetype().chunk(chunk.chunk_index());
    for (const ecs::ComponentTypeId component : components_) {
        Column column;
        const i32 index = chunk.archetype().column_of(component);
        if (index >= 0) {
            column.base = static_cast<const u8*>(view.column(static_cast<u32>(index)));
            column.version = chunk.version(component);
        }
        if (Status pushed = columns_.push_back(column); !pushed) {
            return pushed;
        }
    }
    const Span<const NetworkIdentity> identities = chunk.read<NetworkIdentity>(identity_);
    const Span<const ecs::Entity> entities = chunk.entities();
    for (u32 row = 0; row < chunk.count(); ++row) {
        Record record;
        record.id = identities[row].network_id();
        record.entity = entities[row];
        record.owner = identities[row].owner();
        record.prefab = identities[row].prefab;
        record.columns = first;
        record.row = row;
        if (!by_id_.insert(record.id.value(), static_cast<u32>(records_.size())) ||
            !records_.push_back(record)) {
            return fail(ErrorCode::OutOfMemory, "networking: could not index a replicated entity");
        }
    }
    return ok();
}

Status WorldReplicator::gather(u64 tick) noexcept {
    (void)tick;
    if (!initialized_) {
        return fail(ErrorCode::Unavailable, "networking: the replicator is not initialised");
    }
    gathered_version_ = world_->version();
    columns_.clear();
    records_.clear();
    by_id_.clear();

    ecs::QueryDesc desc(*allocator_);
    if (Status termed = desc.read(identity_); !termed) {
        return termed;
    }
    ecs::Query query(*world_, std::move(desc));
    Status failed = ok();
    if (Status walked = query.for_each_chunk([&](ecs::QueryChunk& chunk) noexcept {
            if (failed) {
                failed = gather_chunk(&chunk);
            }
        });
        !walked) {
        return walked;
    }
    if (!failed) {
        return failed;
    }
    return place_subjects();
}

Status WorldReplicator::place_subjects() noexcept {
    if (config_.subject == nullptr) {
        return ok();
    }
    for (const Record& record : records_) {
        RelevanceSubject subject;
        subject.id = record.id;
        subject.owner = record.owner;
        config_.subject(config_.subject_user, *world_, record.entity, subject);
        subject.id = record.id;
        subject.owner = record.owner;
        if (Status placed = interest_.place(subject); !placed) {
            return placed;
        }
    }
    // Remove the dead from the index, or relevance would keep offering them — including entities
    // that lived and died between two peers' joins and that no peer ever knew.
    for (const u64 id : placed_) {
        if (!by_id_.contains(id)) {
            interest_.remove(NetworkId::make(static_cast<u16>(id >> 48), id));
        }
    }
    placed_.clear();
    for (const Record& record : records_) {
        if (Status pushed = placed_.push_back(record.id.value()); !pushed) {
            return pushed;
        }
    }
    return ok();
}

// --- Build ---------------------------------------------------------------------------------------

Status WorldReplicator::collect_relevant(Peer& peer, Array<Candidate>& out) noexcept {
    out.clear();
    left_.clear();
    if (config_.subject != nullptr && peer.has_interest) {
        Expected<RelevanceDelta, Error> evaluated = interest_.evaluate(peer.interest, out, left_);
        return evaluated ? ok() : Status{make_unexpected(evaluated.error())};
    }
    for (const Record& record : records_) {
        Candidate candidate;
        candidate.id = record.id;
        candidate.rule =
            record.owner == peer.peer ? RelevanceRule::Ownership : RelevanceRule::AlwaysRelevant;
        if (Status pushed = out.push_back(candidate); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status WorldReplicator::retire_unknown(Peer& peer, Span<const NetworkId> left) noexcept {
    peer.scratch.clear();
    for (const auto& entry : peer.sync) {
        if (entry.value.state != EntitySync::State::Despawning && !by_id_.contains(entry.key)) {
            if (Status pushed = peer.scratch.push_back(entry.key); !pushed) {
                return pushed;
            }
        }
    }
    for (const NetworkId id : left) {
        if (Status pushed = peer.scratch.push_back(id.value()); !pushed) {
            return pushed;
        }
    }
    for (const u64 id : peer.scratch) {
        EntitySync* sync = peer.sync.find(id);
        if (sync == nullptr) {
            continue;
        }
        sync->state = EntitySync::State::Despawning;
        sync->synced_version = 0;
        sync->pending_snapshot = 0;
        peer.baseline.forget(NetworkId::make(static_cast<u16>(id >> 48), id));
    }
    return ok();
}

bool WorldReplicator::chunk_is_clean(const Record& record, u64 synced_version) const noexcept {
    if (synced_version == 0) {
        return false;
    }
    for (u32 schema = 0; schema < components_.size(); ++schema) {
        const Column& column = columns_[record.columns + schema];
        if (column.base != nullptr && column.version >= synced_version) {
            return false;
        }
    }
    return true;
}

bool WorldReplicator::compute_masks(Peer& peer, const Record& record) noexcept {
    bool any = false;
    const bool owner = record.owner == peer.peer;
    for (u32 schema = 0; schema < schemas_->size(); ++schema) {
        const Column& column = columns_[record.columns + schema];
        ChangeMask mask = 0;
        if (column.base != nullptr) {
            const CompiledSchema& compiled = schemas_->at(schema);
            const void* instance =
                column.base + (static_cast<usize>(record.row) * compiled.instance_size());
            mask = compiled.changes(instance, peer.baseline.acknowledged(record.id, schema), owner);
        }
        any = any || mask != 0;
        if (!peer.masks.push_back(mask)) {
            return any;
        }
    }
    return any;
}

Status WorldReplicator::classify_one(Peer& peer, const Candidate& candidate, u32 index,
                                     u64 tick) noexcept {
    const Record& record = records_[index];
    EntitySync* sync = peer.sync.find(record.id.value());
    const bool spawn = sync == nullptr || sync->state != EntitySync::State::Live;
    if (!spawn && chunk_is_clean(record, sync->synced_version)) {
        ++peer.report.skipped_by_version;
        return ok();
    }
    ++peer.report.examined;
    Dirty dirty;
    dirty.record = index;
    dirty.spawn = spawn;
    dirty.masks = static_cast<u32>(peer.masks.size());
    const bool changed = compute_masks(peer, record);
    if (!spawn) {
        ++peer.report.entities_compared;
    }
    if (!spawn && !changed) {
        // Compared and equal: the acknowledged baseline IS this version, with no ack to wait for.
        // The next tick skips the entity until something in its chunk is written.
        sync->synced_version = gathered_version_;
        (void)peer.masks.resize(dirty.masks);
        return ok();
    }
    if (Status noted = scheduler_.note_changed(record.id, tick); !noted) {
        return noted;
    }
    if (!peer.dirty_index.insert(record.id.value(), static_cast<u32>(peer.dirty.size())) ||
        !peer.dirty.push_back(dirty) || !peer.candidates.push_back(candidate)) {
        return fail(ErrorCode::OutOfMemory, "networking: could not record a dirty entity");
    }
    return ok();
}

Status WorldReplicator::classify(Peer& peer, Span<const Candidate> relevant, u64 tick) noexcept {
    peer.dirty.clear();
    peer.dirty_index.clear();
    peer.masks.clear();
    peer.candidates.clear();
    for (const Candidate& candidate : relevant) {
        const u32* index = by_id_.find(candidate.id.value());
        if (index == nullptr) {
            continue;
        }
        if (Status classified = classify_one(peer, candidate, *index, tick); !classified) {
            return classified;
        }
    }
    return ok();
}

u32 WorldReplicator::cost_bits(const Peer& peer, const Dirty& dirty) const noexcept {
    u32 bits = dirty.spawn ? kSpawnBits : 0;
    for (u32 schema = 0; schema < schemas_->size(); ++schema) {
        const ChangeMask mask = peer.masks[dirty.masks + schema];
        if (mask != 0) {
            bits += kUpdateOverheadBits + schemas_->at(schema).bits_for(mask);
        }
    }
    return bits;
}

u32 WorldReplicator::cost_of(void* user, NetworkId id, u32 precision_percent) noexcept {
    (void)precision_percent;
    auto* self = static_cast<WorldReplicator*>(user);
    const Peer& peer = *self->costing_;
    const u32* index = peer.dirty_index.find(id.value());
    if (index == nullptr) {
        return 0;
    }
    return (self->cost_bits(peer, peer.dirty[*index]) + 7) / 8;
}

Status WorldReplicator::write_despawns(Peer& peer, SnapshotRecord& record, u64 budget_bits,
                                       u64& used) noexcept {
    for (const auto& entry : peer.sync) {
        if (entry.value.state != EntitySync::State::Despawning ||
            used + kDespawnBits > budget_bits) {
            continue;
        }
        if (!writer_.add_despawn(NetworkId::make(static_cast<u16>(entry.key >> 48), entry.key)) ||
            !record.despawned.push_back(entry.key)) {
            return fail(ErrorCode::OutOfMemory, "networking: could not write a despawn");
        }
        used += kDespawnBits;
        ++peer.report.despawns;
    }
    return ok();
}

Status WorldReplicator::write_components(Peer& peer, const Dirty& dirty) noexcept {
    const Record& entity = records_[dirty.record];
    const bool owner = entity.owner == peer.peer;
    for (u32 schema = 0; schema < schemas_->size(); ++schema) {
        const Column& column = columns_[entity.columns + schema];
        if (column.base == nullptr || peer.masks[dirty.masks + schema] == 0) {
            continue;
        }
        const void* instance =
            column.base + (static_cast<usize>(entity.row) * schemas_->at(schema).instance_size());
        Expected<u32, Error> written =
            writer_.add_update(entity.id, schema, instance, owner, peer.baseline);
        if (!written) {
            return make_unexpected(written.error());
        }
        ++peer.report.updates;
    }
    return ok();
}

Status WorldReplicator::write_entity(Peer& peer, SnapshotRecord& record, const Dirty& dirty,
                                     u64 tick) noexcept {
    const Record& entity = records_[dirty.record];
    if (dirty.spawn) {
        SpawnEvent spawn;
        spawn.id = entity.id;
        spawn.prefab = entity.prefab;
        spawn.owner = entity.owner;
        if (!writer_.add_spawn(spawn) || !record.spawned.push_back(entity.id.value())) {
            return fail(ErrorCode::OutOfMemory, "networking: could not write a spawn");
        }
        ++peer.report.spawns;
    }
    if (Status components = write_components(peer, dirty); !components) {
        return components;
    }
    EntitySync* sync = peer.sync.find(entity.id.value());
    if (sync == nullptr) {
        Expected<EntitySync*, Error> inserted = peer.sync.insert(entity.id.value(), EntitySync{});
        if (!inserted) {
            return make_unexpected(inserted.error());
        }
        sync = *inserted;
    }
    if (dirty.spawn) {
        sync->state = EntitySync::State::Spawning;
    }
    sync->pending_snapshot = record.id;
    sync->pending_version = gathered_version_;
    if (!record.sent.push_back(entity.id.value())) {
        return fail(ErrorCode::OutOfMemory, "networking: could not record a sent entity");
    }
    ++peer.report.written;
    return scheduler_.note_sent(peer.peer, entity.id, tick);
}

Status WorldReplicator::write(Peer& peer, u64 tick, const BandwidthBudget& budget) noexcept {
    const SnapshotId snapshot = peer.next++;
    SnapshotRecord& record = *peer.history[snapshot % kSnapshotHistory];
    record.id = snapshot;
    record.version = gathered_version_;
    record.acknowledged = false;
    record.sent.clear();
    record.spawned.clear();
    record.despawned.clear();
    if (Status opened = writer_.open(snapshot, tick, false); !opened) {
        return opened;
    }
    const u64 budget_bits = static_cast<u64>(budget.bytes_per_tick) * 8;
    u64 used = kHeaderBits;
    if (Status despawned = write_despawns(peer, record, budget_bits, used); !despawned) {
        return despawned;
    }
    for (const ScheduledEntry& entry : selected_) {
        const u32* dirty_index = peer.dirty_index.find(entry.id.value());
        if (entry.dormancy_signal || dirty_index == nullptr) {
            continue;
        }
        const Dirty& dirty = peer.dirty[*dirty_index];
        const u32 bits = cost_bits(peer, dirty);
        if (used + bits > budget_bits) {
            ++peer.report.deferred;
            continue;
        }
        if (Status written = write_entity(peer, record, dirty, tick); !written) {
            return written;
        }
        used += bits;
    }
    peer.baseline.note_sent(snapshot);
    return writer_.close();
}

Expected<Span<const u8>, Error> WorldReplicator::build(PeerId peer_id, u64 tick,
                                                       const BandwidthBudget& budget) noexcept {
    Peer* peer = find(peer_id);
    if (peer == nullptr) {
        return fail(ErrorCode::NotFound, "networking: no such peer");
    }
    peer->report = ReplicationReport{};
    peer->report.budget = budget.bytes_per_tick;
    if (Status collected = collect_relevant(*peer, relevant_); !collected) {
        return make_unexpected(collected.error());
    }
    peer->report.relevant = static_cast<u32>(relevant_.size());
    if (Status retired = retire_unknown(*peer, left_.span()); !retired) {
        return make_unexpected(retired.error());
    }
    if (Status classified = classify(*peer, relevant_.span(), tick); !classified) {
        return make_unexpected(classified.error());
    }
    peer->report.candidates = static_cast<u32>(peer->candidates.size());

    selected_.clear();
    costing_ = peer;
    Expected<SchedulerReport, Error> scheduled = scheduler_.select(
        peer_id, peer->candidates.span(), tick, budget, &cost_of, this, selected_);
    costing_ = nullptr;
    if (!scheduled) {
        return make_unexpected(scheduled.error());
    }
    peer->report.scheduler = *scheduled;
    if (Status written = write(*peer, tick, budget); !written) {
        return make_unexpected(written.error());
    }
    peer->report.snapshot = peer->next - 1;
    peer->report.bytes = static_cast<u32>(writer_.bytes().size());
    return writer_.bytes();
}

void WorldReplicator::acknowledge_one(Peer& peer, SnapshotId id) noexcept {
    SnapshotRecord& record = *peer.history[id % kSnapshotHistory];
    if (record.id != id || record.acknowledged) {
        return;
    }
    record.acknowledged = true;
    peer.baseline.acknowledge_exactly(id);
    for (const u64 spawned : record.spawned) {
        EntitySync* sync = peer.sync.find(spawned);
        if (sync != nullptr && sync->state == EntitySync::State::Spawning) {
            sync->state = EntitySync::State::Live;
        }
    }
    for (const u64 despawned : record.despawned) {
        const EntitySync* sync = peer.sync.find(despawned);
        if (sync != nullptr && sync->state == EntitySync::State::Despawning) {
            (void)peer.sync.remove(despawned);
        }
    }
    for (const u64 sent : record.sent) {
        EntitySync* sync = peer.sync.find(sent);
        if (sync != nullptr && sync->pending_snapshot == id) {
            sync->synced_version = record.version;
            sync->pending_snapshot = 0;
        }
    }
}

void WorldReplicator::acknowledge(PeerId peer_id, SnapshotId newest, u32 previous) noexcept {
    Peer* peer = find(peer_id);
    if (peer == nullptr || newest == 0) {
        return;
    }
    acknowledge_one(*peer, newest);
    for (u32 back = 1; back <= 32 && back < newest; ++back) {
        if ((previous & (1U << (back - 1))) != 0) {
            acknowledge_one(*peer, newest - back);
        }
    }
}

const ReplicationReport& WorldReplicator::last_report(PeerId peer) const noexcept {
    static const ReplicationReport kNone{};
    const Peer* state = find(peer);
    return state == nullptr ? kNone : state->report;
}

// --- ReplicaWorld --------------------------------------------------------------------------------

ReplicaWorld::ReplicaWorld(Allocator& allocator, ecs::World& world,
                           const SchemaSet& schemas) noexcept
    : world_(&world),
      schemas_(&schemas),
      reader_(allocator, schemas),
      components_(allocator),
      bound_(allocator),
      spawns_(allocator),
      despawns_(allocator) {}

Status ReplicaWorld::initialize() noexcept {
    Expected<ecs::ComponentTypeId, Error> identity = network_identity_component(*world_);
    if (!identity) {
        return make_unexpected(identity.error());
    }
    identity_ = *identity;
    return bind_schemas(*world_, *schemas_, components_);
}

ecs::Entity ReplicaWorld::entity_of(NetworkId id) const noexcept {
    const u64* bits = bound_.find(id.value());
    return bits == nullptr ? ecs::Entity{} : unpack(*bits);
}

Expected<ecs::Entity, Error> ReplicaWorld::obtain(NetworkId id) noexcept {
    if (const u64* bits = bound_.find(id.value()); bits != nullptr) {
        return unpack(*bits);
    }
    // The spawn record precedes its entity's first update in the snapshot stream, so it is already
    // in `spawns_` when the update is decoded.
    SpawnEvent known;
    known.id = id;
    for (const SpawnEvent& spawn : spawns_) {
        if (spawn.id == id) {
            known = spawn;
        }
    }
    Expected<ecs::Entity, Error> created = world_->create();
    if (!created) {
        return created;
    }
    NetworkIdentity identity;
    identity.id = id.value();
    identity.prefab = known.prefab;
    identity.owner_slot = known.owner.slot();
    identity.owner_generation = known.owner.generation();
    if (Status added = world_->add(*created, identity_, &identity); !added) {
        return make_unexpected(added.error());
    }
    if (!bound_.insert(id.value(), created->bits())) {
        return fail(ErrorCode::OutOfMemory, "networking: could not bind a replicated entity");
    }
    return created;
}

Status ReplicaWorld::apply_update(void* user, NetworkId id, u32 schema, Span<const u8> decoded,
                                  ChangeMask mask) noexcept {
    auto* self = static_cast<ReplicaWorld*>(user);
    Expected<ecs::Entity, Error> entity = self->obtain(id);
    if (!entity) {
        return make_unexpected(entity.error());
    }
    const ecs::ComponentTypeId component = self->components_[schema];
    if (!self->world_->has(*entity, component)) {
        if (Status added = self->world_->add(*entity, component, decoded.data()); !added) {
            return added;
        }
    }
    auto* target = static_cast<u8*>(self->world_->get_mut(*entity, component));
    if (target == nullptr) {
        return fail(ErrorCode::Internal, "networking: a replicated component has no storage");
    }
    const CompiledSchema& compiled = self->schemas_->at(schema);
    for (u32 field = 0; field < compiled.field_count(); ++field) {
        if ((mask & (1ULL << field)) == 0) {
            continue;
        }
        const CompiledField& layout = compiled.field(field);
        std::memcpy(target + layout.offset, decoded.data() + layout.offset, layout.size);
    }
    ++self->updates_;
    return ok();
}

void ReplicaWorld::despawn(NetworkId id) noexcept {
    const u64* bits = bound_.find(id.value());
    if (bits == nullptr) {
        return;
    }
    (void)world_->destroy(unpack(*bits));
    (void)bound_.remove(id.value());
}

Status ReplicaWorld::spawn(const SpawnEvent& spawn) noexcept {
    Expected<ecs::Entity, Error> entity = obtain(spawn.id);
    return entity ? ok() : Status{make_unexpected(entity.error())};
}

void ReplicaWorld::note_applied(SnapshotId snapshot) noexcept {
    if (newest_ == 0) {
        newest_ = snapshot;
        previous_ = 0;
        return;
    }
    const u32 shift = snapshot - newest_;
    // The old newest becomes bit `shift - 1`; anything shifted past 32 is forgotten.
    if (shift > 32) {
        previous_ = 0;
    } else if (shift == 32) {
        previous_ = 1U << 31;
    } else {
        previous_ = (previous_ << shift) | (1U << (shift - 1));
    }
    newest_ = snapshot;
}

Expected<SnapshotHeader, Error> ReplicaWorld::apply(Span<const u8> snapshot) noexcept {
    BitReader peek(snapshot);
    u64 id = 0;
    if (!peek.read(32, id)) {
        return fail(ErrorCode::OutOfRange, "networking: a snapshot shorter than its header");
    }
    SnapshotHeader header;
    if (static_cast<SnapshotId>(id) <= newest_) {
        ++stale_;
        header.snapshot = static_cast<SnapshotId>(id);
        return header;
    }
    spawns_.clear();
    despawns_.clear();
    if (Status read = reader_.read(snapshot, header, spawns_, despawns_, &apply_update, this);
        !read) {
        return make_unexpected(read.error());
    }
    for (const NetworkId gone : despawns_) {
        despawn(gone);
    }
    for (const SpawnEvent& arrived : spawns_) {
        if (Status spawned = spawn(arrived); !spawned) {
            return make_unexpected(spawned.error());
        }
    }
    note_applied(header.snapshot);
    return header;
}

// --- Digest --------------------------------------------------------------------------------------

namespace {

constexpr u64 kFnvOffset = 0xCBF2'9CE4'8422'2325ULL;
constexpr u64 kFnvPrime = 0x0000'0100'0000'01B3ULL;

void fold(u64& hash, Span<const u8> bytes) noexcept {
    for (const u8 byte : bytes) {
        hash = (hash ^ byte) * kFnvPrime;
    }
}

void fold_u64(u64& hash, u64 value) noexcept {
    u8 bytes[8];
    for (u32 index = 0; index < 8; ++index) {
        bytes[index] = static_cast<u8>(value >> (index * 8));
    }
    fold(hash, Span<const u8>(bytes, 8));
}

/// Every field that reaches every peer, as the schema encodes it. Owner-only and except-owner
/// fields differ between peers by design, so they are not part of what "the same world" means.
[[nodiscard]] ChangeMask shared_fields(const CompiledSchema& schema) noexcept {
    ChangeMask mask = 0;
    for (u32 field = 0; field < schema.field_count(); ++field) {
        if (schema.field(field).filter == TargetFilter::Everyone) {
            mask |= 1ULL << field;
        }
    }
    return mask;
}

struct DigestEntry {
    u64 id = 0;
    u64 hash = 0;
};

}  // namespace

u64 replicated_digest(const ecs::World& world, const SchemaSet& schemas) noexcept {
    Allocator& allocator = world.allocator();
    Array<ecs::ComponentTypeId> components(allocator);
    const ecs::ComponentInfo* identity_info = world.components().find(kNetworkIdentityComponent);
    if (identity_info == nullptr || !bind_schemas(world, schemas, components)) {
        return 0;
    }
    const ecs::ComponentTypeId identity = identity_info->id;
    Array<DigestEntry> entries(allocator);
    Array<u8> bits(allocator);
    auto& mutable_world = const_cast<ecs::World&>(world);
    ecs::QueryDesc desc(allocator);
    if (!desc.read(identity)) {
        return 0;
    }
    ecs::Query query(mutable_world, std::move(desc));
    (void)query.for_each_chunk([&](ecs::QueryChunk& chunk) noexcept {
        const Span<const NetworkIdentity> identities = chunk.read<NetworkIdentity>(identity);
        for (u32 row = 0; row < chunk.count(); ++row) {
            DigestEntry entry;
            entry.id = identities[row].id;
            entry.hash = kFnvOffset;
            fold_u64(entry.hash, identities[row].id);
            for (u32 schema = 0; schema < schemas.size(); ++schema) {
                const i32 column = chunk.archetype().column_of(components[schema]);
                if (column < 0) {
                    continue;
                }
                const CompiledSchema& compiled = schemas.at(schema);
                const auto* base = static_cast<const u8*>(
                    chunk.archetype().chunk(chunk.chunk_index()).column(static_cast<u32>(column)));
                bits.clear();
                BitWriter writer(bits);
                (void)compiled.encode_instance(
                    base + (static_cast<usize>(row) * compiled.instance_size()),
                    shared_fields(compiled), writer);
                (void)writer.flush();
                fold_u64(entry.hash, schema);
                fold(entry.hash, bits.span());
            }
            (void)entries.push_back(entry);
        }
    });
    // Sorted by network id, so creation order on either side does not matter.
    std::ranges::sort(
        entries, [](const DigestEntry& a, const DigestEntry& b) noexcept { return a.id < b.id; });
    u64 digest = kFnvOffset;
    fold_u64(digest, entries.size());
    for (const DigestEntry& entry : entries) {
        fold_u64(digest, entry.hash);
    }
    return digest;
}

}  // namespace cy::net
