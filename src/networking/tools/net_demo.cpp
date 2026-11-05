// SPDX-License-Identifier: MIT
#include "net_demo.h"

#include <cy/ecs/query.h>

#include <cmath>
#include <cstddef>
#include <cstring>

namespace cy::net::demo {
namespace {

// A placeholder component registered first on the client, so the client's component ids differ
// from the server's and only the schema set's binding by type can line them up.
struct ClientOnly {
    u32 frame = 0;
};

[[nodiscard]] reflect::FieldInfo field(const char* name, u32 id, reflect::FieldKind kind,
                                       u32 offset, u32 size) noexcept {
    reflect::FieldInfo out;
    out.name = name;
    out.id = reflect::FieldId(id);
    out.kind = kind;
    out.offset = offset;
    out.size = size;
    return out;
}

void replicate_as(reflect::FieldInfo& field, const char* encoder, const char* parameters,
                  const char* condition) noexcept {
    field.attributes.declared = field.attributes.declared | reflect::AttributeKind::Replicated;
    field.attributes.replicated = reflect::ReplicatedAttribute{encoder, parameters, condition};
}

[[nodiscard]] const reflect::TypeInfo& client_only_type() noexcept {
    static reflect::FieldInfo frame =
        field("frame", 0x4E31, reflect::FieldKind::U32, 0, static_cast<u32>(sizeof(u32)));
    static reflect::TypeInfo info = [] {
        reflect::TypeInfo built;
        built.name = "cy::net::demo::ClientOnly";
        built.id = reflect::TypeId(0x4E30);
        built.size = static_cast<u32>(sizeof(ClientOnly));
        built.alignment = static_cast<u32>(alignof(ClientOnly));
        built.trivially_relocatable = true;
        built.fields = &frame;
        built.field_count = 1;
        return built;
    }();
    return info;
}

}  // namespace

const reflect::TypeInfo& unit_type() noexcept {
    static reflect::FieldInfo fields[5];
    static reflect::TypeInfo info;
    static bool built = false;
    if (built) {
        return info;
    }
    fields[0] =
        field("position", 0x4E01, reflect::FieldKind::F32,
              static_cast<u32>(offsetof(Unit, position)), static_cast<u32>(sizeof(Unit::position)));
    fields[0].attributes.declared = reflect::AttributeKind::Range;
    fields[0].attributes.range = reflect::RangeAttribute{-2048.0, 2048.0, 0.0};
    replicate_as(fields[0], "quantised", "bits=20", "on-change");
    fields[1] = field("health", 0x4E02, reflect::FieldKind::U16,
                      static_cast<u32>(offsetof(Unit, health)), 2);
    replicate_as(fields[1], "bitfield", "bits=10", "owner-and-observers");
    fields[2] =
        field("team", 0x4E03, reflect::FieldKind::U16, static_cast<u32>(offsetof(Unit, team)), 2);
    replicate_as(fields[2], "bitfield", "bits=4", "on-change");
    fields[3] = field("orders", 0x4E04, reflect::FieldKind::U32,
                      static_cast<u32>(offsetof(Unit, orders)), 4);
    replicate_as(fields[3], "raw", "", "owner-only");
    fields[4] =
        field("phase", 0x4E05, reflect::FieldKind::F32, static_cast<u32>(offsetof(Unit, phase)), 4);
    info.name = "cy::net::demo::Unit";
    info.id = reflect::TypeId(0x4E00);
    info.size = static_cast<u32>(sizeof(Unit));
    info.alignment = static_cast<u32>(alignof(Unit));
    info.trivially_relocatable = true;
    info.fields = fields;
    info.field_count = 5;
    built = true;
    return info;
}

DemoSchemas::DemoSchemas(Allocator& allocator) noexcept : fields_(allocator), set_(allocator) {
    Expected<ReplicationSchema, SchemaRejection> schema =
        schema_from_reflection(unit_type(), fields_);
    valid_ = schema.has_value() && set_.add(unit_type(), *schema).has_value();
}

DemoServerGame::DemoServerGame(ecs::World& world, WorldReplicator& replicator) noexcept
    : world_(&world),
      replicator_(&replicator),
      units_(world.allocator()),
      avatars_(world.allocator()) {}

Status DemoServerGame::initialize() noexcept {
    Expected<ecs::ComponentTypeId, Error> unit =
        world_->components().register_reflected(unit_type());
    if (!unit) {
        return make_unexpected(unit.error());
    }
    unit_ = *unit;
    return replicator_->initialize();
}

Status DemoServerGame::populate(u32 count) noexcept {
    const u32 side = static_cast<u32>(std::ceil(std::sqrt(static_cast<f64>(count))));
    for (u32 index = 0; index < count; ++index) {
        Expected<ecs::Entity, Error> entity =
            world_->create(Span<const ecs::ComponentTypeId>(&unit_, 1));
        if (!entity) {
            return make_unexpected(entity.error());
        }
        Unit unit;
        const u32 column = index % side;
        const u32 row = index / side;
        unit.position[0] = (static_cast<f32>(column) * 8.0F) - 400.0F;
        unit.position[1] = (static_cast<f32>(row) * 8.0F) - 400.0F;
        unit.team = static_cast<u16>(index % 4);
        unit.phase = static_cast<f32>(index) * 0.37F;
        if (Status set = world_->set(*entity, unit_, unit); !set) {
            return set;
        }
        if (Expected<NetworkId, Error> id = replicator_->replicate(*entity, 1, PeerId{}); !id) {
            return make_unexpected(id.error());
        }
        if (Status pushed = units_.push_back(*entity); !pushed) {
            return pushed;
        }
    }
    return ok();
}

u32 DemoServerGame::avatars() const noexcept {
    return static_cast<u32>(avatars_.size());
}

void DemoServerGame::wander(u64 tick) noexcept {
    const f32 time = static_cast<f32>(tick) * 0.05F;
    for (const ecs::Entity entity : units_) {
        Unit* unit = world_->get_mut<Unit>(entity, unit_);
        if (unit == nullptr) {
            continue;
        }
        // A small circle per unit, phase-shifted: every unit changes position every tick, which is
        // the worst case for bandwidth and the one the budget has to hold under.
        unit->position[0] += 0.4F * std::cos(time + unit->phase);
        unit->position[1] += 0.4F * std::sin(time + unit->phase);
    }
}

void DemoServerGame::apply(const AppliedCommand& command) noexcept {
    if (!command.target.valid()) {
        return;
    }
    Unit* unit = world_->get_mut<Unit>(command.target, unit_);
    const ecs::Entity owner_avatar = [&]() noexcept {
        for (const Avatar& avatar : avatars_) {
            if (avatar.peer == command.peer) {
                return avatar.entity;
            }
        }
        return ecs::Entity{};
    }();
    // Authority: a peer may only steer its own avatar. A command naming somebody else's is ignored,
    // which is `networking-and-replication`'s "clients SHALL never be trusted for authoritative
    // state" in one comparison.
    if (unit == nullptr || !(command.target == owner_avatar)) {
        return;
    }
    if (command.command.type == kMoveCommand &&
        command.command.payload_size == sizeof(MovePayload)) {
        MovePayload move;
        std::memcpy(&move, command.command.payload, sizeof(move));
        unit->position[0] += static_cast<f32>(move.dx) * 0.25F;
        unit->position[1] += static_cast<f32>(move.dy) * 0.25F;
        unit->orders += 1;
    } else if (command.command.type == kHealCommand) {
        unit->health = 100;
    }
}

void DemoServerGame::simulate(void* self, ecs::World& world, u64 tick,
                              Span<const AppliedCommand> commands) noexcept {
    (void)world;
    auto* game = static_cast<DemoServerGame*>(self);
    if (game->wandering_) {
        game->wander(tick);
    }
    for (const AppliedCommand& command : commands) {
        if (command.kind == AppliedCommand::Kind::Command) {
            game->apply(command);
        }
    }
}

void DemoServerGame::on_peer(void* self, PeerId peer, bool joined) noexcept {
    auto* game = static_cast<DemoServerGame*>(self);
    if (!joined) {
        for (usize index = 0; index < game->avatars_.size(); ++index) {
            if (game->avatars_[index].peer == peer) {
                (void)game->world_->destroy(game->avatars_[index].entity);
                game->avatars_.remove_unordered(index);
                return;
            }
        }
        return;
    }
    Expected<ecs::Entity, Error> entity =
        game->world_->create(Span<const ecs::ComponentTypeId>(&game->unit_, 1));
    if (!entity) {
        return;
    }
    Unit unit;
    unit.position[0] = static_cast<f32>(peer.slot()) * 16.0F;
    unit.team = 9;
    (void)game->world_->set(*entity, game->unit_, unit);
    if (game->replicator_->replicate(*entity, 2, peer).has_value()) {
        (void)game->avatars_.push_back(Avatar{peer, *entity});
    }
}

Status register_client_components(ecs::World& world) noexcept {
    if (Expected<ecs::ComponentTypeId, Error> first =
            world.components().register_reflected(client_only_type());
        !first) {
        return make_unexpected(first.error());
    }
    Expected<ecs::ComponentTypeId, Error> unit = world.components().register_reflected(unit_type());
    return unit ? ok() : Status{make_unexpected(unit.error())};
}

NetworkId avatar_of(const ecs::World& world, PeerId owner) noexcept {
    const ecs::ComponentInfo* identity = world.components().find(kNetworkIdentityComponent);
    if (identity == nullptr || !owner.valid()) {
        return {};
    }
    NetworkId found;
    auto& mutable_world = const_cast<ecs::World&>(world);
    ecs::QueryDesc desc(world.allocator());
    if (!desc.read(identity->id)) {
        return {};
    }
    ecs::Query query(mutable_world, std::move(desc));
    (void)query.for_each_chunk([&](ecs::QueryChunk& chunk) noexcept {
        for (const NetworkIdentity& one : chunk.read<NetworkIdentity>(identity->id)) {
            if (one.owner() == owner) {
                found = one.network_id();
            }
        }
    });
    return found;
}

bool key_from_hex(const char* text, crypto::Key& out) noexcept {
    if (text == nullptr || std::strlen(text) != 64) {
        return false;
    }
    for (u32 index = 0; index < 32; ++index) {
        u32 byte = 0;
        for (u32 half = 0; half < 2; ++half) {
            const char c = text[(index * 2) + half];
            u32 nibble = 0;
            if (c >= '0' && c <= '9') {
                nibble = static_cast<u32>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                nibble = static_cast<u32>(c - 'a') + 10;
            } else if (c >= 'A' && c <= 'F') {
                nibble = static_cast<u32>(c - 'A') + 10;
            } else {
                return false;
            }
            byte = (byte << 4) | nibble;
        }
        out.bytes[index] = static_cast<u8>(byte);
    }
    return true;
}

void key_to_hex(const crypto::Key& key, char (&out)[65]) noexcept {
    constexpr char kDigits[] = "0123456789abcdef";
    for (u32 index = 0; index < 32; ++index) {
        const usize at = static_cast<usize>(index) * 2;
        out[at] = kDigits[key.bytes[index] >> 4];
        out[at + 1] = kDigits[key.bytes[index] & 0x0FU];
    }
    out[64] = '\0';
}

NetCommand move(NetworkId avatar, u64 tick, i16 dx, i16 dy) noexcept {
    NetCommand command;
    command.tick = tick;
    command.type = kMoveCommand;
    command.target = avatar;
    const MovePayload payload{dx, dy};
    command.payload_size = static_cast<u16>(sizeof(payload));
    std::memcpy(command.payload, &payload, sizeof(payload));
    return command;
}

}  // namespace cy::net::demo
