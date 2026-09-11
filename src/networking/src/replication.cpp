#include <cy/networking/replication.h>

#include <algorithm>
#include <cstring>

namespace cy::net {
namespace {

/// Bit widths on the wire. Named rather than spelled at each call site, because the writer and the
/// reader must agree and a literal in two places is a literal that drifts.
inline constexpr u32 kTagBits = 2;
inline constexpr u32 kIdBits = 64;
inline constexpr u32 kPrefabBits = 32;
inline constexpr u32 kSnapshotBits = 32;
inline constexpr u32 kTickBits = 64;
inline constexpr u32 kSchemaBits = 8;
inline constexpr u32 kPeerSlotBits = 32;
inline constexpr u32 kPeerGenerationBits = 32;

}  // namespace

// --- PeerBaseline --------------------------------------------------------------------------------

PeerBaseline::PeerBaseline(Allocator& allocator, const SchemaSet& schemas) noexcept
    : schemas_(&schemas),
      entries_(allocator),
      by_id_(allocator),
      free_(allocator),
      bytes_(allocator),
      known_(allocator) {}

PeerBaseline::Entry* PeerBaseline::locate(NetworkId id, u32 schema) noexcept {
    const u32* head = by_id_.find(id.value());
    for (u32 index = head == nullptr ? kNoEntry : *head; index != kNoEntry;
         index = entries_[index].next) {
        if (entries_[index].schema == schema) {
            return &entries_[index];
        }
    }
    return nullptr;
}

const PeerBaseline::Entry* PeerBaseline::locate(NetworkId id, u32 schema) const noexcept {
    const u32* head = by_id_.find(id.value());
    for (u32 index = head == nullptr ? kNoEntry : *head; index != kNoEntry;
         index = entries_[index].next) {
        if (entries_[index].schema == schema) {
            return &entries_[index];
        }
    }
    return nullptr;
}

Expected<u32, Error> PeerBaseline::obtain(NetworkId id, u32 schema, u32 size) noexcept {
    u32 index = kNoEntry;
    for (usize slot = 0; slot < free_.size(); ++slot) {
        if (entries_[free_[slot]].size >= size) {
            index = free_[slot];
            free_.remove_unordered(slot);
            break;
        }
    }
    if (index == kNoEntry) {
        const u32 offset = static_cast<u32>(bytes_.size());
        if (Status grown = bytes_.resize(bytes_.size() + (static_cast<usize>(size) * 2)); !grown) {
            return fail(ErrorCode::OutOfMemory, "networking: could not grow a peer's baseline");
        }
        Entry fresh;
        fresh.offset = offset;
        fresh.pending_offset = offset + size;
        fresh.size = size;
        if (Status pushed = entries_.push_back(fresh); !pushed) {
            return fail(ErrorCode::OutOfMemory, "networking: could not add a baseline entry");
        }
        index = static_cast<u32>(entries_.size() - 1);
    }

    Entry& entry = entries_[index];
    entry.id_value = id.value();
    entry.schema = schema;
    entry.pending_snapshot = 0;
    entry.has_acknowledged = false;

    u32* head = by_id_.find(id.value());
    if (head == nullptr) {
        Expected<u32*, Error> inserted = by_id_.insert(id.value(), index);
        if (!inserted) {
            return fail(ErrorCode::OutOfMemory, "networking: could not index a baseline entry");
        }
        entry.next = kNoEntry;
        return index;
    }
    entry.next = *head;
    *head = index;
    return index;
}

const void* PeerBaseline::acknowledged(NetworkId id, u32 schema) const noexcept {
    const Entry* entry = locate(id, schema);
    if (entry == nullptr || !entry->has_acknowledged) {
        return nullptr;
    }
    return bytes_.data() + entry->offset;
}

Status PeerBaseline::record_sent(NetworkId id, u32 schema, SnapshotId snapshot,
                                 const void* instance, u32 size) noexcept {
    Entry* entry = locate(id, schema);
    if (entry == nullptr) {
        Expected<u32, Error> index = obtain(id, schema, size);
        if (!index) {
            return make_unexpected(index.error());
        }
        entry = &entries_[index.value()];
    }
    if (size > entry->size) {
        return fail(ErrorCode::InvalidArgument,
                    "networking: a component larger than the baseline slot compiled for it");
    }
    std::memcpy(static_cast<void*>(bytes_.data() + entry->pending_offset), instance, size);
    entry->pending_snapshot = snapshot;
    return ok();
}

void PeerBaseline::acknowledge(SnapshotId snapshot) noexcept {
    acknowledged_ = std::max(snapshot, acknowledged_);
    for (auto& entry : entries_) {
        if (entry.pending_snapshot == 0 || entry.pending_snapshot > snapshot) {
            continue;
        }
        std::memcpy(static_cast<void*>(bytes_.data() + entry.offset),
                    static_cast<const void*>(bytes_.data() + entry.pending_offset), entry.size);
        entry.has_acknowledged = true;
        entry.pending_snapshot = 0;
    }
}

void PeerBaseline::forget(NetworkId id) noexcept {
    u32* head = by_id_.find(id.value());
    if (head != nullptr) {
        for (u32 index = *head; index != kNoEntry;) {
            const u32 next = entries_[index].next;
            entries_[index].id_value = 0;
            entries_[index].has_acknowledged = false;
            entries_[index].pending_snapshot = 0;
            entries_[index].next = kNoEntry;
            (void)free_.push_back(index);
            index = next;
        }
        (void)by_id_.remove(id.value());
    }
    note_unknown(id);
}

bool PeerBaseline::knows(NetworkId id) const noexcept {
    return known_.contains(id.value());
}

Status PeerBaseline::note_known(NetworkId id) noexcept {
    return known_.insert(id.value());
}

void PeerBaseline::note_unknown(NetworkId id) noexcept {
    (void)known_.remove(id.value());
}

void PeerBaseline::clear() noexcept {
    entries_.clear();
    by_id_.clear();
    free_.clear();
    bytes_.clear();
    known_.clear();
    acknowledged_ = 0;
    sent_ = 0;
}

// --- SnapshotWriter ------------------------------------------------------------------------------

SnapshotWriter::SnapshotWriter(Allocator& allocator, const SchemaSet& schemas) noexcept
    : schemas_(&schemas), payload_(allocator), writer_(payload_) {}

Status SnapshotWriter::open(SnapshotId snapshot, u64 tick, bool full_baseline) noexcept {
    payload_.clear();
    writer_ = BitWriter(payload_);
    cost_ = SnapshotCost{};
    snapshot_ = snapshot;
    tick_ = tick;
    full_ = full_baseline;
    open_ = true;

    if (Status written = writer_.write(snapshot, kSnapshotBits); !written) {
        return written;
    }
    if (Status written = writer_.write(tick, kTickBits); !written) {
        return written;
    }
    return writer_.write(full_baseline ? 1U : 0U, 1);
}

Status SnapshotWriter::add_spawn(const SpawnEvent& spawn) noexcept {
    if (!open_) {
        return fail(ErrorCode::Unavailable, "networking: the snapshot is not open");
    }
    if (Status written = writer_.write(static_cast<u64>(SnapshotTag::Spawn), kTagBits); !written) {
        return written;
    }
    if (Status written = writer_.write(spawn.id.value(), kIdBits); !written) {
        return written;
    }
    if (Status written = writer_.write(spawn.prefab, kPrefabBits); !written) {
        return written;
    }
    if (Status written = writer_.write(spawn.owner.slot(), kPeerSlotBits); !written) {
        return written;
    }
    if (Status written = writer_.write(spawn.owner.generation(), kPeerGenerationBits); !written) {
        return written;
    }
    ++cost_.spawns;
    return ok();
}

Status SnapshotWriter::add_despawn(NetworkId id) noexcept {
    if (!open_) {
        return fail(ErrorCode::Unavailable, "networking: the snapshot is not open");
    }
    if (Status written = writer_.write(static_cast<u64>(SnapshotTag::Despawn), kTagBits);
        !written) {
        return written;
    }
    if (Status written = writer_.write(id.value(), kIdBits); !written) {
        return written;
    }
    ++cost_.despawns;
    return ok();
}

Expected<u32, Error> SnapshotWriter::add_update(NetworkId id, u32 schema, const void* instance,
                                                bool owner, PeerBaseline& baseline) noexcept {
    if (!open_) {
        return fail(ErrorCode::Unavailable, "networking: the snapshot is not open");
    }
    if (schema >= schemas_->size()) {
        return fail(ErrorCode::OutOfRange, "networking: no such compiled schema");
    }
    const CompiledSchema& compiled = schemas_->at(schema);
    const void* against = full_ ? nullptr : baseline.acknowledged(id, schema);
    const ChangeMask mask = compiled.changes(instance, against, owner);
    if (mask == 0) {
        return 0U;
    }

    const u64 before = writer_.bits_written();
    if (Status written = writer_.write(static_cast<u64>(SnapshotTag::Update), kTagBits); !written) {
        return make_unexpected(written.error());
    }
    if (Status written = writer_.write(id.value(), kIdBits); !written) {
        return make_unexpected(written.error());
    }
    if (Status written = writer_.write(schema, kSchemaBits); !written) {
        return make_unexpected(written.error());
    }
    Expected<u32, Error> encoded = compiled.encode_instance(instance, mask, writer_);
    if (!encoded) {
        return encoded;
    }
    if (Status recorded =
            baseline.record_sent(id, schema, snapshot_, instance, compiled.instance_size());
        !recorded) {
        return make_unexpected(recorded.error());
    }

    const u32 spent = static_cast<u32>(writer_.bits_written() - before);
    ++cost_.components;
    cost_.payload_bits += spent;
    cost_.theoretical_bits += kTagBits + kIdBits + kSchemaBits + compiled.theoretical_bits();
    return spent;
}

Status SnapshotWriter::close() noexcept {
    if (!open_) {
        return fail(ErrorCode::Unavailable, "networking: the snapshot is not open");
    }
    if (Status written = writer_.write(static_cast<u64>(SnapshotTag::End), kTagBits); !written) {
        return written;
    }
    open_ = false;
    return writer_.flush();
}

// --- SnapshotReader ------------------------------------------------------------------------------

SnapshotReader::SnapshotReader(Allocator& allocator, const SchemaSet& schemas) noexcept
    : schemas_(&schemas), scratch_(allocator) {}

Status SnapshotReader::read(Span<const u8> payload, SnapshotHeader& header,
                            Array<SpawnEvent>& spawns, Array<NetworkId>& despawns, ApplyFn apply,
                            void* user) noexcept {
    BitReader reader(payload);
    u64 value = 0;
    if (!reader.read(kSnapshotBits, value)) {
        return fail(ErrorCode::OutOfRange, "networking: a snapshot shorter than its header");
    }
    header.snapshot = static_cast<SnapshotId>(value);
    if (!reader.read(kTickBits, value)) {
        return fail(ErrorCode::OutOfRange, "networking: a snapshot shorter than its header");
    }
    header.tick = value;
    if (!reader.read(1, value)) {
        return fail(ErrorCode::OutOfRange, "networking: a snapshot shorter than its header");
    }
    header.full_baseline = value != 0;
    header.spawns = 0;
    header.despawns = 0;
    header.updates = 0;

    for (;;) {
        u64 tag = 0;
        if (!reader.read(kTagBits, tag)) {
            return fail(ErrorCode::OutOfRange, "networking: a snapshot with no terminator");
        }
        if (static_cast<SnapshotTag>(tag) == SnapshotTag::End) {
            return ok();
        }
        u64 id_value = 0;
        if (!reader.read(kIdBits, id_value)) {
            return fail(ErrorCode::OutOfRange, "networking: a snapshot record cut short");
        }
        const NetworkId id = NetworkId::make(static_cast<u16>(id_value >> 48), id_value);

        if (static_cast<SnapshotTag>(tag) == SnapshotTag::Despawn) {
            if (Status pushed = despawns.push_back(id); !pushed) {
                return pushed;
            }
            ++header.despawns;
            continue;
        }
        if (static_cast<SnapshotTag>(tag) == SnapshotTag::Spawn) {
            SpawnEvent spawn;
            spawn.id = id;
            u64 prefab = 0;
            u64 slot = 0;
            u64 generation = 0;
            if (!reader.read(kPrefabBits, prefab) || !reader.read(kPeerSlotBits, slot) ||
                !reader.read(kPeerGenerationBits, generation)) {
                return fail(ErrorCode::OutOfRange, "networking: a spawn record cut short");
            }
            spawn.prefab = static_cast<u32>(prefab);
            spawn.owner = PeerId::make(static_cast<u32>(slot), static_cast<u32>(generation));
            if (Status pushed = spawns.push_back(spawn); !pushed) {
                return pushed;
            }
            ++header.spawns;
            continue;
        }

        u64 schema = 0;
        if (!reader.read(kSchemaBits, schema)) {
            return fail(ErrorCode::OutOfRange, "networking: an update record cut short");
        }
        if (schema >= schemas_->size()) {
            return fail(ErrorCode::OutOfRange,
                        "networking: an update naming a schema this build does not have; the "
                        "schema set identity should have refused this peer at connection");
        }
        const CompiledSchema& compiled = schemas_->at(static_cast<u32>(schema));
        if (Status sized = scratch_.resize(compiled.instance_size()); !sized) {
            return sized;
        }
        std::memset(static_cast<void*>(scratch_.data()), 0, scratch_.size());
        ChangeMask mask = 0;
        if (Status decoded =
                compiled.decode_instance(reader, static_cast<void*>(scratch_.data()), mask);
            !decoded) {
            return decoded;
        }
        if (apply != nullptr) {
            if (Status applied = apply(user, id, static_cast<u32>(schema), scratch_.span(), mask);
                !applied) {
                return applied;
            }
        }
        ++header.updates;
    }
}

// --- ReferenceResolver ---------------------------------------------------------------------------

Status ReferenceResolver::bind(NetworkId id, ecs::Entity local) noexcept {
    if (u64* existing = bindings_.find(id.value()); existing != nullptr) {
        *existing = (static_cast<u64>(local.generation()) << 32) | local.index();
        return ok();
    }
    Expected<u64*, Error> inserted =
        bindings_.insert(id.value(), (static_cast<u64>(local.generation()) << 32) | local.index());
    return inserted ? ok() : Status{make_unexpected(inserted.error())};
}

void ReferenceResolver::unbind(NetworkId id) noexcept {
    (void)bindings_.remove(id.value());
}

ecs::Entity ReferenceResolver::resolve(NetworkId id) noexcept {
    const u64* packed = bindings_.find(id.value());
    if (packed == nullptr) {
        ++unresolved_;
        return {};
    }
    return ecs::Entity::make(static_cast<u32>(*packed & 0xFFFF'FFFFULL),
                             static_cast<u32>(*packed >> 32));
}

Status ReferenceResolver::defer(NetworkId holder, u32 field_offset, NetworkId target) noexcept {
    Deferred deferred;
    deferred.holder = holder;
    deferred.target = target;
    deferred.field_offset = field_offset;
    return pending_.push_back(deferred);
}

u32 ReferenceResolver::resolve_pending(NetworkId id, Array<u32>& offsets,
                                       Array<NetworkId>& holders) noexcept {
    u32 resolved = 0;
    usize index = 0;
    while (index < pending_.size()) {
        if (!(pending_[index].target == id)) {
            ++index;
            continue;
        }
        if (offsets.push_back(pending_[index].field_offset) &&
            holders.push_back(pending_[index].holder)) {
            ++resolved;
        }
        pending_.remove_unordered(index);
    }
    return resolved;
}

}  // namespace cy::net
