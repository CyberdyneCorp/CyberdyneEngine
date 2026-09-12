// Water streaming: one body for the whole world, a few resident segments, and a server profile that
// keeps the queries and drops the surfaces. M10 task 2.3, and `water`'s "Water streaming"
// requirement.
//
// An integration suite because it drives a real `world::CellEventQueue` with real cell identities
// and real footprints, which is the only way "evicted with them" is a measurement rather than a
// claim about two lifetimes that are supposed to agree.
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL — reported in `verified_failing`: `release_segment()`
// was changed to drop a segment on the first release rather than when its reference count reached
// zero. "a segment straddling two cells survives one of them being evicted" went red immediately.
// The reference count was restored.

#include <cy/test/test.h>

#include <cy/water/streaming.h>
#include <cy/water/system.h>

#include "fixtures.h"

namespace test = cy::water::test;
using cy::water::WaterBodyId;
using cy::water::WaterPayload;
using cy::water::WaterRegistry;
using cy::water::WaterSegmentKey;
using cy::water::WaterStreaming;
using cy::water::WaterStreamingReport;

namespace {

/// Make a cell resident, with the channel mask a profile would have asked for.
[[nodiscard]] cy::Status stream_cell(cy::world::CellEventQueue& events, WaterStreaming& streaming,
                                     const cy::world::CellCoord& coord, cy::world::ChannelMask mask,
                                     cy::world::CellEventKind kind) noexcept {
    const cy::world::CellId cell = cy::world::cell_id_of(test::partition(), coord);
    if (cy::Status declared = streaming.declare_cell(cell, coord); !declared) {
        return declared;
    }
    cy::world::CellEvent event;
    event.kind = kind;
    event.cell = cell;
    event.channels = mask;
    return events.emit(event);
}

[[nodiscard]] cy::Status evict_cell(cy::world::CellEventQueue& events,
                                    const cy::world::CellCoord& coord) noexcept {
    cy::world::CellEvent event;
    event.kind = cy::world::CellEventKind::Evicted;
    event.cell = cy::world::cell_id_of(test::partition(), coord);
    event.channels = cy::world::ChannelMask::none();
    return events.emit(event);
}

}  // namespace

CY_TEST_CASE("a continental river is one body whose runtime data is segmented") {
    WaterRegistry registry(test::allocator());
    // A river three kilometres long: twenty-four 128 m cells of world, and one identity.
    cy::water::WaterBodyDesc desc = test::river_desc();
    desc.bounds = cy::water::WaterBounds{0.0, -10.0, 0.0, 3072.0, 20.0, 128.0};
    desc.segment_metres = 128.0F;
    const WaterBodyId river = *registry.add(desc);

    WaterStreaming streaming(test::allocator(), registry, test::partition());
    cy::world::CellEventQueue events(test::allocator());
    CY_REQUIRE(streaming.attach(events, 50).has_value());

    // Three cells at opposite ends of the river and one in the middle.
    const cy::i32 cell_columns[3] = {0, 11, 23};
    for (const cy::i32 x : cell_columns) {
        CY_REQUIRE(stream_cell(events, streaming, cy::world::CellCoord{x, 0, 0, 0},
                               cy::world::ChannelMask::all(), cy::world::CellEventKind::Resident)
                       .has_value());
    }
    const auto report = streaming.tick();
    CY_REQUIRE(report.has_value());
    CY_CHECK_EQ(report->cells_bound, 3u);
    CY_CHECK_EQ(report->segments_created, 3u);

    // ONE BODY. A query anywhere along it identifies the same body, and the segments are keyed by
    // it — there is no per-cell river object, and no type in this module could hold one.
    const cy::f64 probes[3] = {64.0, 1472.0, 2944.0};
    for (const cy::f64 x : probes) {
        const WaterSegmentKey key = WaterStreaming::segment_of(river, desc.segment_metres,
                                                               cy::world::WorldVec3d{x, 0.0, 64.0});
        CY_CHECK(key.body == river);
        CY_CHECK(streaming.is_resident(key));
    }
    // And the parts nobody streamed are not resident, which is the other half of "logically for the
    // whole world while only its nearby segments are resident".
    CY_CHECK_FALSE(streaming.is_resident(WaterStreaming::segment_of(
        river, desc.segment_metres, cy::world::WorldVec3d{2000.0, 0.0, 64.0})));
    CY_CHECK_EQ(streaming.resident_segments(), 3u);
}

CY_TEST_CASE("a dedicated server keeps the queries and physics and drops the surfaces") {
    WaterRegistry registry(test::allocator());
    const WaterBodyId ocean = *registry.add(test::ocean_desc());

    WaterStreaming client(test::allocator(), registry, test::partition());
    client.set_profile(cy::world::WorldProfile::Client);
    WaterStreaming server(test::allocator(), registry, test::partition());
    server.set_profile(cy::world::WorldProfile::DedicatedServer);

    cy::world::CellEventQueue client_events(test::allocator());
    cy::world::CellEventQueue server_events(test::allocator());
    CY_REQUIRE(client.attach(client_events, 50).has_value());
    CY_REQUIRE(server.attach(server_events, 50).has_value());

    const cy::world::CellCoord coord{0, 0, 0, 0};
    CY_REQUIRE(stream_cell(client_events, client, coord,
                           cy::world::profile_channels(cy::world::WorldProfile::Client),
                           cy::world::CellEventKind::Resident)
                   .has_value());
    CY_REQUIRE(stream_cell(server_events, server, coord,
                           cy::world::profile_channels(cy::world::WorldProfile::DedicatedServer),
                           cy::world::CellEventKind::Resident)
                   .has_value());
    CY_REQUIRE(client.tick().has_value());
    CY_REQUIRE(server.tick().has_value());

    // THE SERVER PROFILE STILL STREAMS WATER: the channel is in its mask, because it floats boats
    // and paths swimmers.
    CY_CHECK(cy::world::profile_channels(cy::world::WorldProfile::DedicatedServer)
                 .has(cy::world::Channel::Water));
    CY_CHECK_EQ(client.resident_segments(), 1u);
    CY_CHECK_EQ(server.resident_segments(), 1u);

    const cy::world::WorldVec3d at{64.0, 0.0, 64.0};
    // What it keeps: the query state and the physics representation.
    CY_CHECK(server.payload_resident(ocean, at, WaterPayload::Query));
    CY_CHECK(server.payload_resident(ocean, at, WaterPayload::Physics));
    CY_CHECK(server.payload_resident(ocean, at, WaterPayload::Shoreline));
    // What it drops: the surface geometry, the foam and the shore audio.
    CY_CHECK_FALSE(server.payload_resident(ocean, at, WaterPayload::Surface));
    CY_CHECK_FALSE(server.payload_resident(ocean, at, WaterPayload::Foam));
    CY_CHECK_FALSE(server.payload_resident(ocean, at, WaterPayload::Audio));
    // The client keeps all of it.
    CY_CHECK(client.payload_resident(ocean, at, WaterPayload::Surface));

    // And the difference is measurable rather than documented: a server's segment is smaller.
    CY_CHECK_LT(server.bytes(), client.bytes());
}

CY_TEST_CASE("a cell without the water channel binds nothing") {
    WaterRegistry registry(test::allocator());
    CY_REQUIRE(registry.add(test::ocean_desc()).has_value());
    WaterStreaming streaming(test::allocator(), registry, test::partition());
    cy::world::CellEventQueue events(test::allocator());
    CY_REQUIRE(streaming.attach(events, 50).has_value());

    cy::world::ChannelMask geometry_only;
    geometry_only.set(cy::world::Channel::Geometry);
    geometry_only.set(cy::world::Channel::Textures);
    CY_REQUIRE(stream_cell(events, streaming, cy::world::CellCoord{0, 0, 0, 0}, geometry_only,
                           cy::world::CellEventKind::Resident)
                   .has_value());
    const auto report = streaming.tick();
    CY_REQUIRE(report.has_value());
    CY_CHECK_EQ(streaming.resident_segments(), 0u);
    CY_CHECK_EQ(report->cells_bound, 0u);

    // And when a source's mask later gains the water channel, the payload streams for the cell that
    // is already resident — the `ChannelsChanged` event, which is why this module consumes it.
    cy::world::CellEvent changed;
    changed.kind = cy::world::CellEventKind::ChannelsChanged;
    changed.cell = cy::world::cell_id_of(test::partition(), cy::world::CellCoord{0, 0, 0, 0});
    changed.channels = cy::world::ChannelMask::all();
    CY_REQUIRE(events.emit(changed).has_value());
    CY_REQUIRE(streaming.tick().has_value());
    CY_CHECK_EQ(streaming.resident_segments(), 1u);
}

CY_TEST_CASE("a segment straddling two cells survives one of them being evicted") {
    WaterRegistry registry(test::allocator());
    // A body whose segments are larger than a cell: one segment covers both cells below.
    cy::water::WaterBodyDesc desc = test::ocean_desc();
    desc.segment_metres = 512.0F;
    CY_REQUIRE(registry.add(desc).has_value());

    WaterStreaming streaming(test::allocator(), registry, test::partition());
    cy::world::CellEventQueue events(test::allocator());
    CY_REQUIRE(streaming.attach(events, 50).has_value());

    const cy::world::CellCoord first{0, 0, 0, 0};
    const cy::world::CellCoord second{1, 0, 0, 0};
    CY_REQUIRE(stream_cell(events, streaming, first, cy::world::ChannelMask::all(),
                           cy::world::CellEventKind::Resident)
                   .has_value());
    CY_REQUIRE(stream_cell(events, streaming, second, cy::world::ChannelMask::all(),
                           cy::world::CellEventKind::Resident)
                   .has_value());
    CY_REQUIRE(streaming.tick().has_value());
    CY_CHECK_EQ(streaming.resident_segments(), 1u);

    // One cell goes; the segment stays, because the other cell still overlaps it.
    CY_REQUIRE(evict_cell(events, first).has_value());
    const auto partial = streaming.tick();
    CY_REQUIRE(partial.has_value());
    CY_CHECK_EQ(partial->cells_released, 1u);
    CY_CHECK_EQ(partial->segments_dropped, 0u);
    CY_CHECK_EQ(streaming.resident_segments(), 1u);

    // The last cell goes and the segment goes with it. "Evicted with them."
    CY_REQUIRE(evict_cell(events, second).has_value());
    const auto final_tick = streaming.tick();
    CY_REQUIRE(final_tick.has_value());
    CY_CHECK_EQ(final_tick->segments_dropped, 1u);
    CY_CHECK_EQ(streaming.resident_segments(), 0u);
    CY_CHECK_EQ(streaming.bound_cells(), 0u);
}

CY_TEST_CASE("deactivating a cell does not drop the water a boat is floating on") {
    WaterRegistry registry(test::allocator());
    CY_REQUIRE(registry.add(test::ocean_desc()).has_value());
    WaterStreaming streaming(test::allocator(), registry, test::partition());
    cy::world::CellEventQueue events(test::allocator());
    CY_REQUIRE(streaming.attach(events, 50).has_value());

    const cy::world::CellCoord coord{0, 0, 0, 0};
    CY_REQUIRE(stream_cell(events, streaming, coord, cy::world::ChannelMask::all(),
                           cy::world::CellEventKind::Resident)
                   .has_value());
    CY_REQUIRE(streaming.tick().has_value());
    CY_CHECK_EQ(streaming.resident_segments(), 1u);

    cy::world::CellEvent deactivated;
    deactivated.kind = cy::world::CellEventKind::Deactivated;
    deactivated.cell = cy::world::cell_id_of(test::partition(), coord);
    deactivated.channels = cy::world::ChannelMask::all();
    CY_REQUIRE(events.emit(deactivated).has_value());
    CY_REQUIRE(streaming.tick().has_value());
    // Deactivation withdraws entities; the cell is still resident and so is its water.
    CY_CHECK_EQ(streaming.resident_segments(), 1u);
}

CY_TEST_CASE("a cell whose footprint holds no water is counted rather than ignored") {
    WaterRegistry registry(test::allocator());
    // A pool in one corner of the world; everything else is dry.
    CY_REQUIRE(registry.add(test::pool_desc()).has_value());

    WaterStreaming streaming(test::allocator(), registry, test::partition());
    cy::world::CellEventQueue events(test::allocator());
    CY_REQUIRE(streaming.attach(events, 50).has_value());
    CY_REQUIRE(stream_cell(events, streaming, cy::world::CellCoord{0, 0, 0, 0},
                           cy::world::ChannelMask::all(), cy::world::CellEventKind::Resident)
                   .has_value());
    const auto report = streaming.tick();
    CY_REQUIRE(report.has_value());
    CY_CHECK_EQ(report->cells_without_water, 1u);
    CY_CHECK_EQ(streaming.resident_segments(), 0u);
}

CY_TEST_CASE("a cell nobody declared a footprint for is skipped, not guessed at") {
    WaterRegistry registry(test::allocator());
    CY_REQUIRE(registry.add(test::ocean_desc()).has_value());
    WaterStreaming streaming(test::allocator(), registry, test::partition());
    cy::world::CellEventQueue events(test::allocator());
    CY_REQUIRE(streaming.attach(events, 50).has_value());

    // `world::CellId` is opaque by requirement, so a footprint cannot be recovered from it. An
    // undeclared cell binds nothing rather than having its extent invented.
    cy::world::CellEvent event;
    event.kind = cy::world::CellEventKind::Resident;
    event.cell = cy::world::CellId{0x1234'5678'9ABC'DEF0ull};
    event.channels = cy::world::ChannelMask::all();
    CY_REQUIRE(events.emit(event).has_value());
    CY_REQUIRE(streaming.tick().has_value());
    CY_CHECK_EQ(streaming.resident_segments(), 0u);
}
