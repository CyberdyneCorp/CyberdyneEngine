#ifndef CY_SAMPLE_CHARACTER_CONTRACT_H
#define CY_SAMPLE_CHARACTER_CONTRACT_H
// contract.h — the host's half of the boundary the game declares in game/Contract.swift. Task 5.1.
//
// Eight component types, each with the fields the host reads or writes, RESOLVED BY NAME. That is
// the whole of what this program knows about the game it is hosting.
//
// --- WHY BY NAME, AND WHY THAT IS NOT A DETAIL ---------------------------------------------------
//
// The ABI's typed accessors address a field by POSITION — `component_get_f32(world, entity, type,
// 2, &out)` — because a name lookup per access would defeat the no-marshalling path they exist to
// be. A host that hard-coded those positions would agree with the game only until somebody
// reordered a Swift struct, and would then read a neighbouring field of the same type with no
// diagnostic anywhere: exactly the silent misread the hot-reload spike measured across a layout
// change.
//
// `CyWorld_T` keeps the field records a registration produced — name, `CyVarType`, byte offset — so
// the host asks for `"velocity"` once, at bring-up, checks the type it got, and holds the offset.
// A renamed field fails `resolve()` with the name it could not find, before a frame runs. A field
// that changed type fails the same way. Neither can be mistaken for a game that behaves oddly.
//
// --- WHY THE HOST READS CHUNK MEMORY RATHER THAN GOING BACK OUT THROUGH `CyInterface` ------------
//
// The engine is on this side of the boundary. `cy::ecs::World::get`/`get_mut` is what the ABI's own
// accessors call underneath, and going out through the C table to come straight back in would add a
// dispatch, a `CyResult` and a second copy of the type check for nothing. The MODULE must use the
// table, because it is the only thing it has; the host must not, because it would be pretending.

#include <cy/abi/host.h>
#include <cy/core/base/expected.h>
#include <cy/core/math/vec.h>
#include <cy/ecs/entity.h>
#include <cy/ecs/world.h>

namespace sample {

/// Where one field sits inside its component's bytes, once its name and type have been checked.
struct FieldSlot {
    cy::u32 offset = 0;
};

/// A component the game registered, and the fields of it this host touches.
///
/// One struct per component rather than a flat list, so that a call site reads
/// `contract.drive.velocity` and cannot pass the character's velocity slot to the camera's
/// component. The type id is `cy::ecs::ComponentTypeId` — the engine's own — because the record the
/// ABI kept carries it.
struct Component {
    cy::ecs::ComponentTypeId type = cy::ecs::kInvalidComponent;

    [[nodiscard]] bool resolved() const noexcept { return type != cy::ecs::kInvalidComponent; }
};

struct PlayerInputComponent : Component {
    FieldSlot move;
    FieldSlot look;
    FieldSlot jump;
    FieldSlot sprint;
};

struct CharacterStateComponent : Component {
    FieldSlot position;
    FieldSlot velocity;
    FieldSlot grounded;
    FieldSlot speed;
};

struct CharacterSpecComponent : Component {
    FieldSlot spawn;
    FieldSlot radius;
    FieldSlot height;
    FieldSlot step_offset;
    FieldSlot max_slope_radians;
};

struct CharacterDriveComponent : Component {
    FieldSlot velocity;
    FieldSlot jump;
    FieldSlot jump_speed;
};

struct CameraSpecComponent : Component {
    FieldSlot offset;
    FieldSlot near_distance;
    FieldSlot far_distance;
    FieldSlot position_half_life;
    FieldSlot rotation_half_life;
    FieldSlot near_field_of_view;
    FieldSlot far_field_of_view;
};

struct CameraIntentComponent : Component {
    FieldSlot focus;
    FieldSlot yaw;
    FieldSlot pitch;
    FieldSlot zoom;
};

struct AudioCueComponent : Component {
    FieldSlot footsteps;
    FieldSlot landings;
    FieldSlot jumps;
};

struct LevelBoxComponent : Component {
    FieldSlot center;
    FieldSlot half_extents;
};

/// The whole boundary, resolved.
struct Contract {
    PlayerInputComponent input;
    CharacterStateComponent state;
    CharacterSpecComponent spec;
    CharacterDriveComponent drive;
    CameraSpecComponent camera_spec;
    CameraIntentComponent camera_intent;
    AudioCueComponent cue;
    LevelBoxComponent level;

    /// Resolve every component and every field against what the module registered.
    ///
    /// `detail` is left pointing at the name that could not be resolved — a string literal from
    /// this file, so it needs no storage and outlives any report. A failure here is a game and a
    /// host that disagree, and saying which name they disagree about is the difference between a
    /// one-line fix and an afternoon.
    [[nodiscard]] cy::Status resolve(const cy::abi::World& binding, const char** detail) noexcept;
};

// --- Reading and writing a resolved field --------------------------------------------------------
//
// Free functions rather than members of `FieldSlot`, because a slot is an offset and knows neither
// the world nor the entity. Every one of them returns the field's default when the entity does not
// have the component: a behaviour that has not been created yet is a normal state during bring-up,
// not an error, and the alternative — an `Expected` at four call sites per tick — would be checked
// by nobody.

[[nodiscard]] cy::f32 read_f32(const cy::ecs::World& world, cy::ecs::Entity entity,
                               const Component& component, FieldSlot field) noexcept;
[[nodiscard]] cy::Vec3 read_vec3(const cy::ecs::World& world, cy::ecs::Entity entity,
                                 const Component& component, FieldSlot field) noexcept;
void write_f32(cy::ecs::World& world, cy::ecs::Entity entity, const Component& component,
               FieldSlot field, cy::f32 value) noexcept;
void write_vec3(cy::ecs::World& world, cy::ecs::Entity entity, const Component& component,
                FieldSlot field, cy::Vec3 value) noexcept;

}  // namespace sample

#endif  // CY_SAMPLE_CHARACTER_CONTRACT_H
