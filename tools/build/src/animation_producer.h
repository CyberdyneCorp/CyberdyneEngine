// SPDX-License-Identifier: MIT
// The `animation` producer's entry point, private to cy_build_content. See animation_producer.cpp.
#pragma once

#include <cy/build/producer.h>

namespace cy::build {

/// `animation`: a `cyanim 1` description and the import bundles it names in; the character's
/// cooked skeleton, its four clips and its compiled program out.
[[nodiscard]] Status produce_animation(NodeContext& context);

}  // namespace cy::build
