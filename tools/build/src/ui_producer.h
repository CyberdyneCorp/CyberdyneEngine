// SPDX-License-Identifier: MIT
// The `ui` producer's entry point, private to cy_build_content. See ui_producer.cpp.
#pragma once

#include <cy/build/producer.h>

namespace cy::build {

/// `ui` — one `.cyui` document in, one cooked bundle out, every document and sheet it reaches
/// discovered.
[[nodiscard]] Status produce_ui(NodeContext& context);

}  // namespace cy::build
