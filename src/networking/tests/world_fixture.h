// SPDX-License-Identifier: MIT
#pragma once
// Replicated components, built the way the reflection generator builds them, and worlds to put them
// in. Issue #82.
//
// The descriptors carry `Replicated(...)` attributes exactly as `CY_REFLECT_FIELD(Replicated(...))`
// would emit them, so `schema_from_reflection()` derives the session's schemas from the type system
// rather than from a hand-written schema table — and the identities below are the stable field ids
// a schema keys on.

#include "fixture.h"

#include <cy/ecs/query.h>
#include <cy/ecs/world.h>
#include <cy/networking/world_replication.h>

#include <cstddef>
#include <cstdlib>

namespace cy::net_test {

/// A fixture's own precondition. Not `CY_ASSERT`: that compiles away in a shipping build, and with
/// it the side effect the condition performs.
inline void fixture_requires(bool condition) noexcept {
    if (!condition) {
        std::abort();
    }
}

/// A unit: where it is, how hurt it is, and the owner's private flags.
struct Body {
    f32 position[3] = {0.0F, 0.0F, 0.0F};
    u16 health = 100;
    u16 team = 0;
    u32 orders = 0;  // owner-only
};

inline constexpr u32 kBodyTypeId = 4200;
inline constexpr u32 kBodyPosition = 4201;
inline constexpr u32 kBodyHealth = 4202;
inline constexpr u32 kBodyTeam = 4203;
inline constexpr u32 kBodyOrders = 4204;

[[nodiscard]] inline reflect::FieldInfo replicated_field(const char* name, u32 id,
                                                         reflect::FieldKind kind, u32 offset,
                                                         u32 size, const char* encoder,
                                                         const char* parameters,
                                                         const char* condition) noexcept {
    reflect::FieldInfo field;
    field.name = name;
    field.id = reflect::FieldId(id);
    field.kind = kind;
    field.offset = offset;
    field.size = size;
    field.attributes.declared = reflect::AttributeKind::Replicated;
    field.attributes.replicated = reflect::ReplicatedAttribute{encoder, parameters, condition};
    return field;
}

[[nodiscard]] inline const reflect::TypeInfo& body_type() noexcept {
    static reflect::FieldInfo fields[4];
    static reflect::TypeInfo info;
    static bool built = false;
    if (built) {
        return info;
    }
    fields[0] = replicated_field("position", kBodyPosition, reflect::FieldKind::F32,
                                 static_cast<u32>(offsetof(Body, position)),
                                 static_cast<u32>(sizeof(Body::position)), "quantised", "bits=20",
                                 "on-change");
    fields[0].attributes.declared =
        reflect::AttributeKind::Replicated | reflect::AttributeKind::Range;
    fields[0].attributes.range = reflect::RangeAttribute{-1000.0, 1000.0, 0.0};
    fields[1] = replicated_field("health", kBodyHealth, reflect::FieldKind::U16,
                                 static_cast<u32>(offsetof(Body, health)), 2, "bitfield", "bits=10",
                                 "owner-and-observers");
    fields[2] = replicated_field("team", kBodyTeam, reflect::FieldKind::U16,
                                 static_cast<u32>(offsetof(Body, team)), 2, "bitfield", "bits=4",
                                 "on-change");
    fields[3] =
        replicated_field("orders", kBodyOrders, reflect::FieldKind::U32,
                         static_cast<u32>(offsetof(Body, orders)), 4, "raw", "", "owner-only");
    info.name = "cy::net_test::Body";
    info.id = reflect::TypeId(kBodyTypeId);
    info.size = static_cast<u32>(sizeof(Body));
    info.alignment = static_cast<u32>(alignof(Body));
    info.trivially_relocatable = true;
    info.fields = fields;
    info.field_count = 4;
    built = true;
    return info;
}

/// A component with no replicated field: present in the world, never on the wire.
struct LocalOnly {
    u32 frame = 0;
};

[[nodiscard]] inline const reflect::TypeInfo& local_only_type() noexcept {
    static reflect::FieldInfo field;
    static reflect::TypeInfo info;
    static bool built = false;
    if (!built) {
        field.name = "frame";
        field.id = reflect::FieldId(4301);
        field.kind = reflect::FieldKind::U32;
        field.size = 4;
        info.name = "cy::net_test::LocalOnly";
        info.id = reflect::TypeId(4300);
        info.size = 4;
        info.alignment = 4;
        info.trivially_relocatable = true;
        info.fields = &field;
        info.field_count = 1;
        built = true;
    }
    return info;
}

/// The session's schema set, derived from reflection.
struct Schemas {
    Array<FieldSchema> body_fields;
    SchemaSet set;

    Schemas() noexcept : body_fields(allocator()), set(allocator()) {
        auto schema = schema_from_reflection(body_type(), body_fields);
        fixture_requires(schema.has_value());
        fixture_requires(set.add(body_type(), *schema).has_value());
    }
};

/// A world with `Body` (and `LocalOnly`) registered.
struct GameWorld {
    ecs::World world;
    ecs::ComponentTypeId body = ecs::kInvalidComponent;
    ecs::ComponentTypeId local = ecs::kInvalidComponent;

    explicit GameWorld(bool local_first = false) noexcept : world(allocator()) {
        fixture_requires(world.initialize().has_value());
        // A client may register its components in another order; the schema set binds by type.
        if (local_first) {
            local = world.components().register_reflected(local_only_type()).value();
        }
        body = world.components().register_reflected(body_type()).value();
        if (!local_first) {
            local = world.components().register_reflected(local_only_type()).value();
        }
    }

    [[nodiscard]] ecs::Entity spawn(f32 x, f32 y, u16 team = 0) noexcept {
        const ecs::ComponentTypeId components[] = {body, local};
        ecs::Entity entity = world.create(components).value();
        Body value;
        value.position[0] = x;
        value.position[1] = y;
        value.team = team;
        fixture_requires(world.set(entity, body, value).has_value());
        return entity;
    }

    [[nodiscard]] Body* body_of(ecs::Entity entity) noexcept {
        return world.get_mut<Body>(entity, body);
    }
    [[nodiscard]] const Body* read(ecs::Entity entity) const noexcept {
        return world.get<Body>(entity, body);
    }
};

}  // namespace cy::net_test
