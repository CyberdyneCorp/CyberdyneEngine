// SPDX-License-Identifier: MIT
// Reading authored fields by name. See authored_fields.h.

#include "authored_fields.h"

namespace cy::gameplay::authored {

const ser::WorldTypeDecl* type_named(const ser::World& world, std::string_view name) noexcept {
    for (const ser::WorldTypeDecl& declared : world.types()) {
        if (world.text(declared.name) == name) {
            return &declared;
        }
    }
    return nullptr;
}

const ser::WorldValue* field_named(const ser::World& world, const ser::WorldTypeDecl& declared,
                                   const ser::WorldComponent& component,
                                   std::string_view name) noexcept {
    for (const ser::WorldFieldDecl& field : declared.fields()) {
        if (world.text(field.name) != name) {
            continue;
        }
        if (const ser::WorldField* held = component.find(field.file_field); held != nullptr) {
            return &held->value;
        }
        return nullptr;
    }
    return nullptr;
}

f32 float_of(const ser::WorldValue* value, f32 fallback) noexcept {
    if (value == nullptr) {
        return fallback;
    }
    switch (value->kind) {
        case ser::WorldValueKind::Float:
            return value->lanes[0];
        case ser::WorldValueKind::Double:
            return static_cast<f32>(value->real);
        case ser::WorldValueKind::Int:
            return static_cast<f32>(value->integer);
        default:
            return fallback;
    }
}

Vec3 vec3_of(const ser::WorldValue* value, Vec3 fallback) noexcept {
    if (value == nullptr || value->kind != ser::WorldValueKind::Vec3) {
        return fallback;
    }
    return Vec3{value->lanes[0], value->lanes[1], value->lanes[2]};
}

bool bool_of(const ser::WorldValue* value, bool fallback) noexcept {
    if (value == nullptr || value->kind != ser::WorldValueKind::Bool) {
        return fallback;
    }
    return value->integer != 0;
}

u64 entity_of(const ser::WorldValue* value) noexcept {
    if (value == nullptr || value->kind != ser::WorldValueKind::Entity) {
        return 0;
    }
    return static_cast<u64>(value->integer);
}

std::string_view text_of(const ser::World& world, const ser::WorldValue* value,
                         std::string_view fallback) noexcept {
    if (value == nullptr || value->kind != ser::WorldValueKind::Text) {
        return fallback;
    }
    const Span<const u8> bytes = world.blob(*value);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

}  // namespace cy::gameplay::authored
