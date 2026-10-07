// SPDX-License-Identifier: MIT
#pragma once
// The deterministic math module, in one include. `cy::core-detmath`, layer 0.
//
// | Header        | What it is                                                                  |
// |---------------|-----------------------------------------------------------------------------|
// | fixed.h       | `Fixed` (Q32.32), `Fixed16` (Q16.16 storage), `Angle`, `WideFixed` (Q64.64) |
// | functions.h   | `sqrt` and the transcendentals, with their declared error bounds            |
// | vec.h         | `FixedVec2`, `FixedVec3`, `Fixed16Vec3`, `Rot2`, `FixedQuat`, `FixedTransform`
// | | shapes.h      | `FixedAabb`, `FixedCircle`, `FixedCapsule2D`, and the exact tests on them   |
// | convert.h     | The float boundary: `from_f32_cooked`, `from_f64_cooked`, `to_f32_relative` |
// | overflow.h    | `OverflowGuard`: wrapping is counted in development builds                  |
// | version.h     | `kKernelVersion`                                                            |
// | digest.h      | The kernel digest every leg reproduces                                      |
// | wide.h        | The 128-bit multiply and divide, and the paths that implement them          |
//
// Linking `cy::core-detmath` defines `CY_DETERMINISM_MATH=1` for the linking target, so
// `determinism::BuildConfiguration::from_build()` reports deterministic math wherever it is linked
// (design §12.1).

#include <cy/core/detmath/convert.h>
#include <cy/core/detmath/digest.h>
#include <cy/core/detmath/fixed.h>
#include <cy/core/detmath/functions.h>
#include <cy/core/detmath/overflow.h>
#include <cy/core/detmath/shapes.h>
#include <cy/core/detmath/vec.h>
#include <cy/core/detmath/version.h>
#include <cy/core/detmath/wide.h>
