// SPDX-License-Identifier: MIT
#pragma once
// The one counter unwrap.cpp and unwrap_cache.cpp share: how many times xatlas has run. Private to
// the importer; `uv2_unwrap_count()` in mesh.h is its public face.

#include <cy/core/base/types.h>

namespace cy::import {

/// Called by `generate_uv2` each time it runs xatlas.
void note_uv2_unwrap() noexcept;

}  // namespace cy::import
