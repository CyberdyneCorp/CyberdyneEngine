// SPDX-License-Identifier: MIT
#pragma once
// A small networked game: units that wander, avatars that players steer with commands. Issue #82.
//
// Shared by the dedicated server (`cy_net_server`), the client (`cy_net_client`), the measurement
// tool (`cy_net_session_bench`) and `tests/test_session.cpp`, so the numbers the tool prints and
// the properties the tests assert are about one game rather than two that happen to look alike.
//
// The replicated component is described the way the reflection generator describes a type —
// `Replicated(...)` attributes on stable field ids — and the session's schema set is derived from
// that description by `schema_from_reflection()`, exactly as a project's own components would be.

#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/ecs/world.h>
#include <cy/networking/crypto.h>
#include <cy/networking/schema.h>
#include <cy/networking/session.h>
#include <cy/networking/world_replication.h>

namespace cy::net::demo {

/// One unit. `phase` is server-only: it declares no `Replicated` attribute and never travels.
struct Unit {
    f32 position[3] = {0.0F, 0.0F, 0.0F};
    u16 health = 100;
    u16 team = 0;
    /// The owner's own orders queue length. Owner-only on the wire.
    u32 orders = 0;
    f32 phase = 0.0F;
};

[[nodiscard]] const reflect::TypeInfo& unit_type() noexcept;

/// Command types, as `gameplay::CommandDeclaration::stable_id` would number them.
inline constexpr u32 kMoveCommand = 0x4D4F'5645U;  // "MOVE"
inline constexpr u32 kHealCommand = 0x4845'414CU;  // "HEAL"

struct MovePayload {
    i16 dx = 0;
    i16 dy = 0;
};

/// The schema set both ends derive.
class DemoSchemas {
public:
    explicit DemoSchemas(Allocator& allocator) noexcept;

    [[nodiscard]] const SchemaSet& set() const noexcept { return set_; }
    [[nodiscard]] bool valid() const noexcept { return valid_; }

private:
    Array<FieldSchema> fields_;
    SchemaSet set_;
    bool valid_ = false;
};

/// The server's game: owns no world, steps the one it is given.
class DemoServerGame {
public:
    DemoServerGame(ecs::World& world, WorldReplicator& replicator) noexcept;

    [[nodiscard]] Status initialize() noexcept;

    /// `count` wandering units, laid out on a lattice. Deterministic: no random source.
    [[nodiscard]] Status populate(u32 count) noexcept;

    /// Units stop wandering; commands still apply. What lets a session settle and be compared.
    void set_wandering(bool wandering) noexcept { wandering_ = wandering; }

    static void simulate(void* self, ecs::World& world, u64 tick,
                         Span<const AppliedCommand> commands) noexcept;
    static void on_peer(void* self, PeerId peer, bool joined) noexcept;

    [[nodiscard]] ecs::ComponentTypeId unit_component() const noexcept { return unit_; }
    [[nodiscard]] u32 avatars() const noexcept;

private:
    void apply(const AppliedCommand& command) noexcept;
    void wander(u64 tick) noexcept;

    ecs::World* world_;
    WorldReplicator* replicator_;
    ecs::ComponentTypeId unit_ = ecs::kInvalidComponent;
    Array<ecs::Entity> units_;
    struct Avatar {
        PeerId peer;
        ecs::Entity entity;
    };
    Array<Avatar> avatars_;
    bool wandering_ = true;
};

/// Register the demo's components in a client world, in a different order from the server's.
[[nodiscard]] Status register_client_components(ecs::World& world) noexcept;

/// The network id of the entity `owner` owns in `world`, found through `NetworkIdentity`. Null
/// until the avatar has been replicated.
[[nodiscard]] NetworkId avatar_of(const ecs::World& world, PeerId owner) noexcept;

/// A key as 64 hexadecimal digits, and back. False on anything else.
[[nodiscard]] bool key_from_hex(const char* text, crypto::Key& out) noexcept;
void key_to_hex(const crypto::Key& key, char (&out)[65]) noexcept;

/// A move command for `avatar` at `tick`.
[[nodiscard]] NetCommand move(NetworkId avatar, u64 tick, i16 dx, i16 dy) noexcept;

}  // namespace cy::net::demo
