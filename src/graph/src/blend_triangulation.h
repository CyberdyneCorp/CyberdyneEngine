// SPDX-License-Identifier: MIT
#pragma once
// The cook-time triangulation of a two-dimensional blend space. Private to src/graph/src: the
// compiler calls it, and the runtime reads the triangles it stored.

#include <cy/graph/lower_pose.h>

namespace cy::graph::pose {

/// A Delaunay triangulation of `samples`' positions, appended to `out` as indices into `samples`,
/// each triangle counter-clockwise. Deterministic: the same positions in the same order give the
/// same triangles in the same order.
///
/// `NotFound` when the samples span no area — fewer than three, or all on one line — because a
/// space with no triangle has no barycentric weights to give. Duplicate positions are the
/// caller's to refuse first: two samples at one point leave one of them in no triangle.
[[nodiscard]] Status triangulate_blend_space(Span<const BlendSample> samples,
                                             Array<BlendTriangle>& out) noexcept;

}  // namespace cy::graph::pose
