// The artefact's own world. M9 section 6.

#include "world.h"

#include <cy/core/determinism/hash.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/type_info.h>
#include <cy/ecs/relationships.h>

#include <cstddef>
#include <cstring>

namespace cy::mp {
namespace {

[[nodiscard]] Allocator& world_allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

/// `Position` as the generator would describe it: two replicated floats. The `Replicated` attribute
/// is what `WriteFirewall::declare_from_reflection()` derives authority from, and it is true of
/// this component — the session puts it on the wire in `net.cpp`.
[[nodiscard]] const reflect::TypeInfo& position_type() noexcept {
    static reflect::FieldInfo fields[2];
    static reflect::TypeInfo info;
    for (reflect::FieldInfo& field : fields) {
        field.kind = reflect::FieldKind::F32;
        field.size = static_cast<u32>(sizeof(f32));
        field.attributes.declared = reflect::AttributeKind::Replicated;
        field.attributes.replicated = reflect::ReplicatedAttribute{"quantised", "bits=16", "all"};
    }
    fields[0].name = "x";
    fields[0].id = reflect::FieldId(2011);
    fields[0].offset = static_cast<u32>(offsetof(Position, x));
    fields[1].name = "y";
    fields[1].id = reflect::FieldId(2012);
    fields[1].offset = static_cast<u32>(offsetof(Position, y));

    info.name = "Position";
    info.id = reflect::TypeId(201);
    info.size = static_cast<u32>(sizeof(Position));
    info.alignment = static_cast<u32>(alignof(Position));
    info.trivially_relocatable = true;
    info.fields = fields;
    info.field_count = 2;
    return info;
}

/// `Health` is NOT replicated in this session — the host owns it and nothing sends it — so its
/// authority comes from the other derivation: a field that declares `RuntimeState` persistence.
/// Two components guarded by two different routes is deliberate; a world where every component
/// derived the same way would not exercise the arming report's own arithmetic.
[[nodiscard]] const reflect::TypeInfo& health_type() noexcept {
    static reflect::FieldInfo fields[2];
    static reflect::TypeInfo info;
    for (reflect::FieldInfo& field : fields) {
        field.attributes.declared = reflect::AttributeKind::Persistence;
        field.attributes.persistence = reflect::PersistenceKind::RuntimeState;
    }
    fields[0].name = "value";
    fields[0].id = reflect::FieldId(2021);
    fields[0].kind = reflect::FieldKind::I32;
    fields[0].offset = static_cast<u32>(offsetof(Health, value));
    fields[0].size = static_cast<u32>(sizeof(i32));
    fields[1].name = "shield";
    fields[1].id = reflect::FieldId(2022);
    fields[1].kind = reflect::FieldKind::U32;
    fields[1].offset = static_cast<u32>(offsetof(Health, shield));
    fields[1].size = static_cast<u32>(sizeof(u32));

    info.name = "Health";
    info.id = reflect::TypeId(202);
    info.size = static_cast<u32>(sizeof(Health));
    info.alignment = static_cast<u32>(alignof(Health));
    info.trivially_relocatable = true;
    info.fields = fields;
    info.field_count = 2;
    return info;
}

}  // namespace

MoveIntent intent_of(u64 tick, u32 player) noexcept {
    return MoveIntent{1, static_cast<i32>((tick + player) % 3U) - 1};
}

// --- The provider
// ---------------------------------------------------------------------------------

determinism::Participates StepAccumulator::participation() const noexcept {
    return determinism::Participates::Rollback | determinism::Participates::Checkpoint |
           determinism::Participates::Hash;
}

Status StepAccumulator::capture(Array<u8>& out) const noexcept {
    for (u32 byte = 0; byte < 8; ++byte) {
        if (Status pushed = out.push_back(static_cast<u8>((value >> (byte * 8U)) & 0xFFU));
            !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status StepAccumulator::restore(Span<const u8> bytes) noexcept {
    if (bytes.size() != 8) {
        return fail(ErrorCode::InvalidArgument, "step accumulator: wrong size");
    }
    value = 0;
    for (u32 byte = 0; byte < 8; ++byte) {
        value |= static_cast<u64>(bytes[byte]) << (byte * 8U);
    }
    return ok();
}

// --- The world
// ------------------------------------------------------------------------------------

GameWorld::GameWorld() noexcept
    : session_(world_allocator(), 0x0910'5EEDULL),
      control_(world_allocator()),
      commands_(world_allocator(), control_),
      world_(world_allocator()),
      schema_(world_allocator()),
      providers_(world_allocator()),
      position_codec_(world_allocator()),
      health_codec_(world_allocator()) {}

bool GameWorld::declare_schema() noexcept {
    const determinism::StateField position_fields[] = {
        {"x", 1, offsetof(Position, x), reflect::FieldKind::F32,
         determinism::SimulationClass::Authoritative, determinism::StateEncoding::Direct},
        {"y", 2, offsetof(Position, y), reflect::FieldKind::F32,
         determinism::SimulationClass::Authoritative, determinism::StateEncoding::Direct},
    };
    const determinism::StateField health_fields[] = {
        {"value", 1, offsetof(Health, value), reflect::FieldKind::I32,
         determinism::SimulationClass::Authoritative, determinism::StateEncoding::Direct},
        {"shield", 2, offsetof(Health, shield), reflect::FieldKind::U32,
         determinism::SimulationClass::Authoritative, determinism::StateEncoding::Direct},
    };
    if (!schema_
             .declare(kPositionSubject, "Position",
                      Span<const determinism::StateField>{position_fields, 2})
             .has_value()) {
        return false;
    }
    if (!schema_
             .declare(kHealthSubject, "Health",
                      Span<const determinism::StateField>{health_fields, 2})
             .has_value()) {
        return false;
    }
    schema_.freeze();
    return position_codec_.compile(schema_, kPositionSubject, determinism::CodecPurpose::Hash)
               .has_value() &&
           health_codec_.compile(schema_, kHealthSubject, determinism::CodecPurpose::Hash)
               .has_value();
}

bool GameWorld::arm_firewall() noexcept {
    // THE TWO THE ECS REGISTERS ITSELF. `World::initialize()` adds the relationship components, and
    // `declare_from_reflection()` can say nothing about them because they have no `TypeInfo` — the
    // category `firewall_arming.h` names. A `Parent` edge is hashed by `<cy/ecs/state_schema.h>`,
    // so re-parenting is an authoritative write and this session says so by hand, with the reason
    // the call refuses to be made without.
    const ecs::ComponentInfo* parent = world_.components().find(ecs::kParentComponentName);
    const ecs::ComponentInfo* children = world_.components().find(ecs::kChildrenComponentName);
    if (parent == nullptr || children == nullptr) {
        return false;
    }
    const gameplay::ManualAuthority by_hand[] = {
        {parent->id, ecs::ComponentAuthority::Authoritative,
         "a Parent edge is hashed, so re-parenting is an authoritative write"},
        {children->id, ecs::ComponentAuthority::Authoritative,
         "the child list is the other half of the same edge and is hashed with it"},
    };
    return gameplay::arm_write_firewall(world_.components(), world_.firewall(),
                                        Span<const gameplay::ManualAuthority>(by_hand, 2), arming_)
        .has_value();
}

bool GameWorld::build() noexcept {
    if (!world_.initialize().has_value()) {
        return false;
    }
    auto registered_position = world_.components().register_reflected(position_type());
    auto registered_health = world_.components().register_reflected(health_type());
    if (!registered_position || !registered_health) {
        return false;
    }
    position_ = *registered_position;
    health_ = *registered_health;

    // ARMED ONCE, AT SESSION START, BEFORE THE FIRST TICK — and before the entities exist, so that
    // `World::create()`'s own lifetime write is admitted under the same rules every later write is.
    if (!arm_firewall()) {
        return false;
    }

    gameplay::CommandDeclaration declaration;
    declaration.name = Name::intern("Move");
    declaration.stable_id = 11;
    declaration.channel = gameplay::channels::movement();
    auto declared = commands_.declare(declaration);
    if (!declared) {
        return false;
    }
    move_ = *declared;

    for (u32 player = 0; player < kPlayers; ++player) {
        auto entity = world_.create();
        if (!entity) {
            return false;
        }
        entities_[player] = *entity;
        if (!world_.add(entities_[player], position_).has_value() ||
            !world_.add(entities_[player], health_).has_value()) {
            return false;
        }
        if (!world_
                 .set(entities_[player], position_,
                      Position{static_cast<f32>(player), static_cast<f32>(player * 2U)})
                 .has_value()) {
            return false;
        }
        if (!world_.set(entities_[player], health_, Health{100, player}).has_value()) {
            return false;
        }

        auto participant = session_.add_participant(gameplay::ParticipantKind::RemoteHuman,
                                                    Name::intern("player"));
        if (!participant) {
            return false;
        }
        participants_[player] = *participant;
        auto source = control_.create_source(gameplay::ControlSourceKind::Human,
                                             participants_[player], Name::intern("input"));
        if (!source) {
            return false;
        }
        sources_[player] = *source;
        if (!control_
                 .bind_entity(sources_[player], gameplay::channels::movement(), entities_[player])
                 .has_value()) {
            return false;
        }
        // ONE PRODUCER PER PARTICIPANT, opened in participant order. The merge order and the
        // per-producer sequence numbers are a four-player session's, and a replay has to reproduce
        // that topology for its log to be byte-identical rather than merely equivalent.
        auto producer = commands_.open_producer(Name::intern("player-input"));
        if (!producer) {
            return false;
        }
        producers_[player] = *producer;
    }

    if (!providers_.add(accumulator_).has_value()) {
        return false;
    }
    providers_.finalize();
    return declare_schema();
}

void GameWorld::set_effect_sink(EffectSink sink, void* user) noexcept {
    effects_ = sink;
    effect_user_ = user;
}

gameplay::GameplayContext GameWorld::context(u64 tick) noexcept {
    gameplay::GameplayContext ctx;
    ctx.session = &session_;
    ctx.services = &session_.services();
    ctx.commands = &commands_;
    ctx.at.tick = tick;
    return ctx;
}

bool GameWorld::offer_intent(u32 player, u64 tick, const MoveIntent& intent) noexcept {
    gameplay::Command command;
    command.type = move_;
    command.participant = participants_[player];
    command.source = sources_[player];
    command.target = entities_[player];
    command.provenance =
        gameplay::Provenance{gameplay::ControlSourceKind::Human, static_cast<u32>(player + 1)};
    command.tick = tick;
    (void)command.set_payload(intent);
    return commands_.producer(producers_[player]).record(command).has_value();
}

replay::LogRecord GameWorld::command_record(u32 player, u64 tick,
                                            const MoveIntent& intent) const noexcept {
    replay::LogRecord record;
    record.kind = replay::RecordKind::Command;
    record.tick = tick;
    record.sequence = player;
    record.command.type = move_;
    record.command.tick = tick;
    record.command.sequence = player;
    record.command.participant = participants_[player];
    record.command.source = sources_[player];
    record.command.target = entities_[player];
    record.command.provenance =
        gameplay::Provenance{gameplay::ControlSourceKind::RemotePeer, static_cast<u32>(player + 1)};
    (void)record.command.set_payload(intent);
    return record;
}

bool GameWorld::feed(Span<const replay::LogRecord> records) noexcept {
    for (const replay::LogRecord& record : records) {
        const gameplay::Command& command = record.command;
        u32 producer = 0;
        for (u32 player = 0; player < kPlayers; ++player) {
            if (participants_[player].bits() == command.participant.bits()) {
                producer = producers_[player];
                break;
            }
        }
        gameplay::Command replayed = command;
        replayed.provenance.kind = gameplay::ControlSourceKind::Replay;
        if (!commands_.producer(producer).record(replayed).has_value()) {
            return false;
        }
    }
    return true;
}

bool GameWorld::step(u64 tick) noexcept {
    commands_.commit(context(tick), tick);
    for (u32 index = 0; index < commands_.committed_count(); ++index) {
        const gameplay::Command& command = commands_.committed(index);
        MoveIntent intent;
        if (!command.read_payload(intent)) {
            return false;
        }
        Position position;
        const void* stored = world_.get(command.target, position_);
        if (stored == nullptr) {
            return false;
        }
        std::memcpy(&position, stored, sizeof(Position));
        // Small integers into `f32`: exact, and deliberately so — see the header.
        position.x += static_cast<f32>(intent.dx);
        position.y += static_cast<f32>(intent.dy);
        if (!world_.set(command.target, position_, position).has_value()) {
            return false;
        }
    }

    // A system that is not driven by commands, so a replay that produced the commands and skipped
    // the systems would still differ.
    if (tick % 7 == 0) {
        for (u32 player = 0; player < kPlayers; ++player) {
            Health health = health_of(player);
            health.value -= 1;
            health.shield = static_cast<u32>(health.value & 0x0F);
            if (!world_.set(entities_[player], health_, health).has_value()) {
                return false;
            }
        }
    }

    accumulator_.value = determinism::fold_hash(accumulator_.value, tick);

    // The effects. `instance` is a function of SIMULATION STATE — the entity — and never a counter,
    // which is the ledger's own requirement.
    if (effects_ != nullptr) {
        for (u32 player = 0; player < kPlayers; ++player) {
            const Position position = position_of(player);
            if (static_cast<i64>(position.x) % 10 != 0) {
                continue;
            }
            ++offered_;
            effects_(effect_user_, kExplosionKind, entities_[player].bits());
        }
    }
    return true;
}

Position GameWorld::position_of(u32 player) const noexcept {
    Position value;
    const void* stored = world_.get(entities_[player], position_);
    if (stored != nullptr) {
        std::memcpy(&value, stored, sizeof(Position));
    }
    return value;
}

Health GameWorld::health_of(u32 player) const noexcept {
    Health value;
    const void* stored = world_.get(entities_[player], health_);
    if (stored != nullptr) {
        std::memcpy(&value, stored, sizeof(Health));
    }
    return value;
}

bool GameWorld::set_health(u32 player, const Health& value) noexcept {
    return world_.set(entities_[player], health_, value).has_value();
}

bool GameWorld::hash(determinism::StateHashTree& tree) noexcept {
    Position positions[kPlayers];
    Health healths[kPlayers];
    u64 ids[kPlayers];
    for (u32 player = 0; player < kPlayers; ++player) {
        positions[player] = position_of(player);
        healths[player] = health_of(player);
        ids[player] = entities_[player].bits();
    }
    if (!tree.begin(determinism::HashLevel::World, 0, "session").has_value() ||
        !tree.begin(determinism::HashLevel::Archetype, kPositionSubject.value, "Position")
             .has_value() ||
        !position_codec_
             .hash_rows(tree, positions, sizeof(Position), kPlayers, ids,
                        determinism::HashDetail::Fields)
             .has_value() ||
        !tree.end().has_value()) {
        return false;
    }
    if (!tree.begin(determinism::HashLevel::Archetype, kHealthSubject.value, "Health")
             .has_value() ||
        !health_codec_
             .hash_rows(tree, healths, sizeof(Health), kPlayers, ids,
                        determinism::HashDetail::Fields)
             .has_value() ||
        !tree.end().has_value()) {
        return false;
    }
    // The provider half, so a restore that forgot it changes the hash rather than passing quietly.
    if (!tree.begin(determinism::HashLevel::Subsystem, 1, "step-accumulator").has_value()) {
        return false;
    }
    tree.mix_u64(accumulator_.value);
    return tree.end().has_value() && tree.end().has_value();
}

u64 GameWorld::root_hash() noexcept {
    determinism::StateHashTree tree(world_allocator());
    if (!hash(tree)) {
        return 0;
    }
    return tree.root_hash();
}

}  // namespace cy::mp
