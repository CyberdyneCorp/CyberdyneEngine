// SPDX-License-Identifier: MIT
// The `vfx` producer's entry point, private to cy_build_content. See vfx_producer.cpp.
#pragma once

#include <cy/build/producer.h>

namespace cy::build {

/// `vfx` — one `.cyvfxdoc` in, one cooked system out, every referenced module discovered.
[[nodiscard]] Status produce_vfx(NodeContext& context);

}  // namespace cy::build
