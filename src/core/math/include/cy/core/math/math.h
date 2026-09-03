#pragma once
// The umbrella header for cy::core-math. Section 3.1.
//
// Include this from a translation unit that wants the whole library. Include the individual headers
// from anything that is included by many others — an engine header that only needs `Vec3` should
// say `<cy/core/math/vec.h>` and not pull the BVH, the curves and the atlas packer into every
// translation unit that reads it.
//
// THE CONVENTIONS, in one place, because they are the part of this module that everything else
// depends on and the part that is expensive to get wrong. Each is asserted numerically in
// tests/test_conventions.cpp; the entries below are a table of contents for that file, not a
// substitute for it.
//
//   Handedness      Right-handed. `cross(kAxisX, kAxisY) == kAxisZ`.
//   Up              +Y.
//   Forward         local −Z. A camera looks down its own −Z; `Transform::forward()` returns it.
//   Rotation        Counter-clockwise about an axis seen from its positive end. Euler order YXZ,
//                   at authoring boundaries only — runtime rotation is a `Quat`.
//   2D screen       Origin top-left, +Y downward.
//   Matrices        Column-major storage, column vectors, `M * v`. `A * B` applies B first.
//                   Translation is the fourth column.
//   Depth           [0, 1], reversed: near → 1, far → 0. Cleared to 0. Opaque compares
//                   GreaterEqual, shadow comparison samplers use Greater.
//   Units           Metres, seconds, kilograms, radians. Degrees only in named converters.
//   Precision       f32 at runtime; f64 for the simulation clock, accumulated time and interchange.
//   Vec3            12 bytes. Never padded to 16.

#include <cy/core/math/batch.h>
#include <cy/core/math/bvh.h>
#include <cy/core/math/color.h>
#include <cy/core/math/curve.h>
#include <cy/core/math/easing.h>
#include <cy/core/math/geometry.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/projection.h>
#include <cy/core/math/quat.h>
#include <cy/core/math/random.h>
#include <cy/core/math/scalar.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/simd.h>
#include <cy/core/math/spatial.h>
#include <cy/core/math/transform.h>
#include <cy/core/math/vec.h>
