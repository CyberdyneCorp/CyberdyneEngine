#pragma once
// The worlds CyberWater's suites are written against.
//
// One ocean, one lake, one river and one pool, because that is the set the specification's
// "Overlapping bodies" and "One abstraction, several backends" scenarios need — and because a
// fixture with one body would let a suite pass while the resolution order was wrong.

#include <cy/core/memory/system_allocator.h>
#include <cy/water/ocean.h>
#include <cy/water/system.h>

namespace cy::water::test {

[[nodiscard]] inline Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

/// A partition with 128 m level-0 cells at the origin — the shape src/world/'s and
/// src/environment/'s own fixtures use, so a cell footprint computed here is the one those modules
/// would compute.
///
/// Returned BY REFERENCE to a function-local constant, not by value. `WaterSystem` and
/// `WaterStreaming` hold the configuration by reference — `FieldStore` does too, for the reason
/// store.h gives — so a fixture returning a temporary would hand every suite a dangling reference
/// the moment the constructor returned. It cost this suite one afternoon; it is written down so it
/// costs the next reader nothing.
[[nodiscard]] inline const world::PartitionConfig& partition() noexcept {
    static const world::PartitionConfig config = [] {
        world::PartitionConfig value;
        value.partition = 1;
        value.base_cell_size = 128.0F;
        value.levels = 3;
        value.level_ratio = 4;
        return value;
    }();
    return config;
}

/// An ocean covering a large square at sea level, lowest priority: it is what answers where nothing
/// else does.
[[nodiscard]] inline WaterBodyDesc ocean_desc() noexcept {
    WaterBodyDesc desc;
    desc.name = "test.ocean";
    desc.type = WaterBodyType::Ocean;
    desc.backend = WaterBackend::Spectral;
    desc.mean_level = 0.0;
    desc.bounds = WaterBounds{-4096.0, -200.0, -4096.0, 4096.0, 20.0, 4096.0};
    desc.priority = 0;
    desc.density = 1025.0F;
    desc.optics = clear_sea_optics();
    desc.segment_metres = 256.0F;
    return desc;
}

/// A silty lake, inland, higher priority than the ocean — a lake inside an ocean's bounds is the
/// ordinary case of an overlap and the one the resolution order has to answer.
[[nodiscard]] inline WaterBodyDesc lake_desc() noexcept {
    WaterBodyDesc desc;
    desc.name = "test.lake";
    desc.type = WaterBodyType::Lake;
    desc.backend = WaterBackend::Spectral;
    desc.mean_level = 12.0;
    desc.bounds = WaterBounds{500.0, -30.0, 500.0, 900.0, 14.0, 900.0};
    desc.priority = 5;
    desc.density = 1000.0F;
    desc.optics = silty_lake_optics();
    desc.segment_metres = 128.0F;
    return desc;
}

/// A river, highest priority: where it has water, it wins over the sea it flows into.
[[nodiscard]] inline WaterBodyDesc river_desc() noexcept {
    WaterBodyDesc desc;
    desc.name = "test.river";
    desc.type = WaterBodyType::River;
    desc.backend = WaterBackend::SplineFlow;
    desc.mean_level = 2.0;
    desc.bounds = WaterBounds{-40.0, -10.0, -40.0, 640.0, 20.0, 240.0};
    desc.priority = 10;
    desc.density = 1000.0F;
    desc.segment_metres = 64.0F;
    return desc;
}

/// A flat pool: the backend with no simulation at all, which every consumer must still be able to
/// query through the one interface.
[[nodiscard]] inline WaterBodyDesc pool_desc() noexcept {
    WaterBodyDesc desc;
    desc.name = "test.pool";
    desc.type = WaterBodyType::Pool;
    desc.backend = WaterBackend::Flat;
    desc.mean_level = 30.0;
    desc.bounds = WaterBounds{2000.0, 28.0, 2000.0, 2010.0, 31.0, 2008.0};
    desc.priority = 20;
    desc.segment_metres = 32.0F;
    return desc;
}

/// A rough sea: ten metres a second over a long fetch.
[[nodiscard]] inline OceanParams rough_sea() noexcept {
    OceanParams params;
    params.wind_speed_mps = 10.0F;
    params.wind_direction_degrees = 0.0F;
    params.fetch_km = 300.0F;
    params.cascades = 4;
    params.trains_per_cascade = 4;
    return params;
}

/// A straight river, four control points, gently downhill. Its width narrows in the middle, which
/// is what the continuity test measures.
[[nodiscard]] inline Status author_straight_river(RiverNetwork& network,
                                                  cy::f32 narrow_width) noexcept {
    RiverControlPoint points[4];
    for (cy::u32 index = 0; index < 4; ++index) {
        points[index].position = world::WorldVec3d{
            static_cast<cy::f64>(index) * 100.0, 10.0 - (static_cast<cy::f64>(index) * 1.0), 100.0};
        points[index].width = 10.0F;
        points[index].depth = 2.0F;
        points[index].flow_speed = 1.0F;
        points[index].turbulence = 0.02F;
    }
    // The narrows, halfway down.
    points[1].width = narrow_width;
    points[2].width = narrow_width;
    return network.add_section(RiverSectionDesc{"trunk", Span<const RiverControlPoint>(points, 4),
                                                kNoSection, 1.0F})
                   .has_value()
               ? ok()
               : fail(ErrorCode::Internal, "water tests: the trunk could not be authored");
}

}  // namespace cy::water::test
