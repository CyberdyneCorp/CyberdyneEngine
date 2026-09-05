#include "contract.h"

#include <cstring>

namespace sample {
namespace {

using cy::abi::ComponentRecord;
using cy::abi::FieldRecord;

/// Find a component the module registered, or fail naming it.
[[nodiscard]] cy::Status bind_component(const cy::abi::World& binding, const char* name,
                                        Component& out, const ComponentRecord** record,
                                        const char** detail) noexcept {
    *detail = name;
    const ComponentRecord* found = binding.find(name);
    if (found == nullptr) {
        return cy::fail(cy::ErrorCode::NotFound,
                        "the game module registered no component of that name");
    }
    out.type = found->id;
    *record = found;
    return cy::ok();
}

/// Find one field by name and check its type. The type check is what makes holding an offset safe:
/// after it, a `f32` slot cannot be pointing at the first four bytes of a `vec3`.
[[nodiscard]] cy::Status bind_field(const cy::abi::World& binding, const ComponentRecord& record,
                                    const char* name, CyVarType expected, FieldSlot& out,
                                    const char** detail) noexcept {
    *detail = name;
    for (cy::u32 index = 0; index < record.field_count; ++index) {
        const FieldRecord* field = binding.field(record, index);
        if (field == nullptr || std::strcmp(field->name, name) != 0) {
            continue;
        }
        if (field->type != expected) {
            return cy::fail(cy::ErrorCode::InvalidArgument,
                            "that field is not of the type this host reads it as");
        }
        out.offset = field->offset;
        return cy::ok();
    }
    return cy::fail(cy::ErrorCode::NotFound, "that component has no field of that name");
}

}  // namespace

cy::Status Contract::resolve(const cy::abi::World& binding, const char** detail) noexcept {
    const ComponentRecord* record = nullptr;

// One macro, and it earns its place: without it this function is a hundred and twenty lines of
// `if (Status s = bind_field(...); !s) { return s; }` in which a single mistyped destination would
// be invisible. Every use names the component, the field's spelling in game/Contract.swift, and the
// type the host reads it as, which is exactly the triple a reviewer has to check.
#define CY_BIND_COMPONENT(NAME, MEMBER)                                                    \
    if (const cy::Status bound = bind_component(binding, NAME, (MEMBER), &record, detail); \
        !bound) {                                                                          \
        return bound;                                                                      \
    }
#define CY_BIND_FIELD(MEMBER, FIELD, NAME, TYPE)                                                   \
    if (const cy::Status bound = bind_field(binding, *record, NAME, TYPE, (MEMBER).FIELD, detail); \
        !bound) {                                                                                  \
        return bound;                                                                              \
    }

    CY_BIND_COMPONENT("PlayerInput", input)
    CY_BIND_FIELD(input, move, "move", CY_VAR_VEC3)
    CY_BIND_FIELD(input, look, "look", CY_VAR_VEC3)
    CY_BIND_FIELD(input, jump, "jump", CY_VAR_F32)
    CY_BIND_FIELD(input, sprint, "sprint", CY_VAR_F32)

    CY_BIND_COMPONENT("CharacterState", state)
    CY_BIND_FIELD(state, position, "position", CY_VAR_VEC3)
    CY_BIND_FIELD(state, velocity, "velocity", CY_VAR_VEC3)
    CY_BIND_FIELD(state, grounded, "grounded", CY_VAR_F32)
    CY_BIND_FIELD(state, speed, "speed", CY_VAR_F32)

    CY_BIND_COMPONENT("CharacterSpec", spec)
    CY_BIND_FIELD(spec, spawn, "spawn", CY_VAR_VEC3)
    CY_BIND_FIELD(spec, radius, "radius", CY_VAR_F32)
    CY_BIND_FIELD(spec, height, "height", CY_VAR_F32)
    CY_BIND_FIELD(spec, step_offset, "stepOffset", CY_VAR_F32)
    CY_BIND_FIELD(spec, max_slope_radians, "maxSlopeRadians", CY_VAR_F32)

    CY_BIND_COMPONENT("CharacterDrive", drive)
    CY_BIND_FIELD(drive, velocity, "velocity", CY_VAR_VEC3)
    CY_BIND_FIELD(drive, jump, "jump", CY_VAR_F32)
    CY_BIND_FIELD(drive, jump_speed, "jumpSpeed", CY_VAR_F32)

    CY_BIND_COMPONENT("CameraSpec", camera_spec)
    CY_BIND_FIELD(camera_spec, offset, "offset", CY_VAR_VEC3)
    CY_BIND_FIELD(camera_spec, near_distance, "nearDistance", CY_VAR_F32)
    CY_BIND_FIELD(camera_spec, far_distance, "farDistance", CY_VAR_F32)
    CY_BIND_FIELD(camera_spec, position_half_life, "positionHalfLife", CY_VAR_F32)
    CY_BIND_FIELD(camera_spec, rotation_half_life, "rotationHalfLife", CY_VAR_F32)
    CY_BIND_FIELD(camera_spec, near_field_of_view, "nearFieldOfView", CY_VAR_F32)
    CY_BIND_FIELD(camera_spec, far_field_of_view, "farFieldOfView", CY_VAR_F32)

    CY_BIND_COMPONENT("CameraIntent", camera_intent)
    CY_BIND_FIELD(camera_intent, focus, "focus", CY_VAR_VEC3)
    CY_BIND_FIELD(camera_intent, yaw, "yaw", CY_VAR_F32)
    CY_BIND_FIELD(camera_intent, pitch, "pitch", CY_VAR_F32)
    CY_BIND_FIELD(camera_intent, zoom, "zoom", CY_VAR_F32)

    CY_BIND_COMPONENT("AudioCue", cue)
    CY_BIND_FIELD(cue, footsteps, "footsteps", CY_VAR_F32)
    CY_BIND_FIELD(cue, landings, "landings", CY_VAR_F32)
    CY_BIND_FIELD(cue, jumps, "jumps", CY_VAR_F32)

    CY_BIND_COMPONENT("LevelBox", level)
    CY_BIND_FIELD(level, center, "center", CY_VAR_VEC3)
    CY_BIND_FIELD(level, half_extents, "halfExtents", CY_VAR_VEC3)

#undef CY_BIND_FIELD
#undef CY_BIND_COMPONENT

    *detail = "";
    return cy::ok();
}

cy::f32 read_f32(const cy::ecs::World& world, cy::ecs::Entity entity, const Component& component,
                 FieldSlot field) noexcept {
    const void* bytes = world.get(entity, component.type);
    if (bytes == nullptr) {
        return 0.0F;
    }
    cy::f32 value = 0.0F;
    std::memcpy(&value, static_cast<const cy::u8*>(bytes) + field.offset, sizeof(value));
    return value;
}

cy::Vec3 read_vec3(const cy::ecs::World& world, cy::ecs::Entity entity, const Component& component,
                   FieldSlot field) noexcept {
    const void* bytes = world.get(entity, component.type);
    if (bytes == nullptr) {
        return cy::Vec3{};
    }
    cy::f32 lanes[3] = {0.0F, 0.0F, 0.0F};
    std::memcpy(lanes, static_cast<const cy::u8*>(bytes) + field.offset, sizeof(lanes));
    return cy::Vec3{lanes[0], lanes[1], lanes[2]};
}

void write_f32(cy::ecs::World& world, cy::ecs::Entity entity, const Component& component,
               FieldSlot field, cy::f32 value) noexcept {
    void* bytes = world.get_mut(entity, component.type);
    if (bytes == nullptr) {
        return;
    }
    std::memcpy(static_cast<cy::u8*>(bytes) + field.offset, &value, sizeof(value));
}

void write_vec3(cy::ecs::World& world, cy::ecs::Entity entity, const Component& component,
                FieldSlot field, cy::Vec3 value) noexcept {
    void* bytes = world.get_mut(entity, component.type);
    if (bytes == nullptr) {
        return;
    }
    const cy::f32 lanes[3] = {value.x, value.y, value.z};
    std::memcpy(static_cast<cy::u8*>(bytes) + field.offset, lanes, sizeof(lanes));
}

}  // namespace sample
