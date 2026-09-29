// SPDX-License-Identifier: MIT
#pragma once
// Reading an authored component's fields by the NAME the world file gave them.
//
// Physics' components are registered in the ECS by name with no reflected type behind them, so a
// play session reads them out of the `.cyworld`'s own type section. These are the helpers the
// session's body, collider and joint readers share; they are internal to this module.

#include <cy/core/math/vec.h>
#include <cy/scene/serialization/worldfile.h>

#include <string_view>

namespace cy::gameplay::authored {

namespace ser = scene::serialization;

/// The declared type of a given name, or null.
[[nodiscard]] const ser::WorldTypeDecl* type_named(const ser::World& world,
                                                   std::string_view name) noexcept;

/// One field of one component, found by the name the file gave it, or null when it is absent.
[[nodiscard]] const ser::WorldValue* field_named(const ser::World& world,
                                                 const ser::WorldTypeDecl& declared,
                                                 const ser::WorldComponent& component,
                                                 std::string_view name) noexcept;

[[nodiscard]] f32 float_of(const ser::WorldValue* value, f32 fallback) noexcept;
[[nodiscard]] Vec3 vec3_of(const ser::WorldValue* value, Vec3 fallback) noexcept;
[[nodiscard]] bool bool_of(const ser::WorldValue* value, bool fallback) noexcept;
/// An entity reference's editor identity, or zero when the field is absent or names none.
[[nodiscard]] u64 entity_of(const ser::WorldValue* value) noexcept;
[[nodiscard]] std::string_view text_of(const ser::World& world, const ser::WorldValue* value,
                                       std::string_view fallback) noexcept;

}  // namespace cy::gameplay::authored
