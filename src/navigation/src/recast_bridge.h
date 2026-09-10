#pragma once
// The declaration of the one function in this engine that Recast implements, and the only reason
// build.cpp does not need to know whether Recast exists.
//
// It is declared unconditionally and DEFINED only in build_recast.cpp, which CMake compiles only
// when `CY_NAVIGATION` is on. `build_tile()` never calls it without checking `recast_available()`
// first, so a build without the option links without the definition and without a stub — a stub
// that answered "unsupported" would be a second place the option is read, and the two would
// eventually disagree.

#include <cy/core/base/expected.h>
#include <cy/navigation/build.h>

namespace cy::navigation::detail {

/// The triangles that survived `NavBuildParams`' declared filters, and the area each carries —
/// `kAreaNull` where the slope limit rejected it.
///
/// It is a struct rather than three parameters because the whole point of it is that BOTH back ends
/// see the SAME triangles: filtering is build.cpp's, once, above the choice of back end. A back end
/// that filtered for itself would make the two produce different meshes from one input, and the
/// suite that compares them would be comparing two different questions.
struct FilteredGeometry {
    Span<const Vec3> vertices;
    Span<const u32> indices;
    Span<const AreaType> areas;
};

[[nodiscard]] Expected<NavTileData, Error> build_tile_recast(Allocator& allocator,
                                                             const NavBuildParams& params,
                                                             const FilteredGeometry& geometry,
                                                             TileCoord coord, const Aabb& bounds,
                                                             NavBuildReport& report) noexcept;

}  // namespace cy::navigation::detail
