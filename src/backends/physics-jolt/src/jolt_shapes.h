#pragma once
// Engine shape descriptions to Jolt shapes. Task 4.2.2.
//
// `physics` — "Jolt as the 3D backend": "engine shapes map to Jolt shapes, with a shape cache so
// identical shapes are shared". The CACHE is the server's (jolt_server.cpp) and is keyed by
// `cy::physics::shape_key()`, which lives in cy_physics so both backends share a shape under
// exactly the same conditions. What is here is the translation of one description.

#include "jolt_common.h"

#include <cy/core/base/expected.h>
#include <cy/servers/physics/shapes.h>

namespace cy::physics::jolt {

/// Resolve a child shape handle to the Jolt shape behind it. Supplied by the server, because the
/// handle table is the server's.
using ChildResolver = const JPH::Shape* (*)(void* context, ShapeHandle child) noexcept;

/// Build the Jolt shape for one description.
///
/// Returns `Unsupported` for the shapes Jolt cannot express as described — a rectangular height
/// field, which Jolt requires to be square — rather than silently building something else.
[[nodiscard]] Expected<JPH::Ref<JPH::Shape>, Error> build_shape(const ShapeDescription& description,
                                                                ChildResolver resolve,
                                                                void* context) noexcept;

}  // namespace cy::physics::jolt
