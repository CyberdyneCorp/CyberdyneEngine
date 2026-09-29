// SPDX-License-Identifier: MIT
// The `navmesh` producer's entry point, private to cy_build_content. See navmesh_producer.cpp.
#pragma once

#include <cy/build/producer.h>

namespace cy::build {

/// `navmesh`: one `.cynavmesh` sidecar in, verified against the world's bake identity, and the
/// cooked navigation mesh out.
[[nodiscard]] Status produce_navmesh(NodeContext& context);

}  // namespace cy::build
