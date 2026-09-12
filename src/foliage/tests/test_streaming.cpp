// Clusters bound to world cells, evicted with them, and independently evictable. M10 task 2.4;
// `foliage`'s "Foliage clusters" streaming requirement.

#include <cy/test/test.h>

#include <cy/foliage/streaming.h>
#include <cy/world/activation.h>
#include <cy/world/cell.h>

#include "fixtures.h"

namespace test = cy::foliage::test;
using cy::foliage::ClusterBounds;
using cy::foliage::ClusterBuilder;
using cy::foliage::ClusterCoord;
using cy::foliage::ClusterId;
using cy::foliage::ClusterStore;
using cy::foliage::FoliageCluster;
using cy::foliage::FoliagePopulationHandle;
using cy::foliage::FoliageStreaming;
using cy::foliage::GrassField;
using cy::foliage::GrassPatch;
using cy::foliage::InstanceFlags;
using cy::foliage::species_id;

namespace {

/// Two clusters and two grass patches per cell, derived from the cell's own identity so the test
/// can tell which cell produced what.
[[nodiscard]] cy::Status loader(void* user, cy::world::CellId cell,
                                const cy::world::PartitionConfig& partition,
                                cy::Array<FoliagePopulationHandle>& out) noexcept {
    auto* calls = static_cast<cy::u32*>(user);
    ++*calls;
    (void)partition;
    for (cy::u32 index = 0; index < 2; ++index) {
        ClusterBounds bounds;
        bounds.min_x = static_cast<cy::f64>(index) * 64.0;
        bounds.min_z = 0.0;
        bounds.max_x = bounds.min_x + 64.0;
        bounds.max_z = 64.0;
        bounds.max_y = 20.0F;
        ClusterBuilder builder(test::allocator(), test::policy(),
                               ClusterId{cell.value ^ (index + 1)},
                               ClusterCoord{static_cast<cy::i32>(index), 0, 0}, bounds);
        if (cy::Status added = builder.add(species_id(test::kPine),
                                           cy::world::WorldVec3d{bounds.min_x + 4.0, 3.0, 4.0},
                                           0.0F, 0.5F, 0, 100, 0.0F, 0.0F, InstanceFlags{});
            !added) {
            return added;
        }
        auto built = builder.finish();
        if (!built) {
            return cy::make_unexpected(built.error());
        }
        FoliagePopulationHandle handle(static_cast<FoliageCluster&&>(built.value()),
                                       test::allocator());
        GrassPatch patch;
        patch.seed = cell.value + index;
        patch.density = 100;
        if (cy::Status pushed = handle.patches.push_back(patch); !pushed) {
            return pushed;
        }
        if (cy::Status pushed = out.push_back(static_cast<FoliagePopulationHandle&&>(handle));
            !pushed) {
            return pushed;
        }
    }
    return cy::ok();
}

[[nodiscard]] cy::world::CellEvent event_for(cy::world::CellId cell, cy::world::CellEventKind kind,
                                             cy::world::ChannelMask channels,
                                             cy::u64 sequence) noexcept {
    cy::world::CellEvent event;
    event.kind = kind;
    event.cell = cell;
    event.channels = channels;
    event.sequence = sequence;
    return event;
}

}  // namespace

CY_TEST_CASE("a region streams in with its clusters and they are evicted when it unloads") {
    ClusterStore clusters(test::allocator());
    GrassField grass(test::allocator());
    FoliageStreaming streaming(test::allocator(), clusters, grass, test::partition());
    cy::world::CellEventQueue events(test::allocator());
    CY_REQUIRE(streaming.attach(events, 40).has_value());
    cy::u32 loader_calls = 0;
    streaming.set_loader(&loader, &loader_calls);

    cy::world::ChannelMask everything = cy::world::ChannelMask::all();
    const cy::world::CellId first =
        cy::world::cell_id_of(test::partition(), cy::world::CellCoord{0, 0, 0, 0});
    const cy::world::CellId second =
        cy::world::cell_id_of(test::partition(), cy::world::CellCoord{1, 0, 0, 0});
    CY_REQUIRE(events.emit(event_for(first, cy::world::CellEventKind::Resident, everything, 1))
                   .has_value());
    CY_REQUIRE(events.emit(event_for(second, cy::world::CellEventKind::Resident, everything, 2))
                   .has_value());

    auto report = streaming.tick();
    CY_REQUIRE(report.has_value());
    CY_CHECK_EQ(report.value().events_drained, 2u);
    CY_CHECK_EQ(report.value().cells_bound, 2u);
    CY_CHECK_EQ(report.value().clusters_loaded, 4u);
    CY_CHECK_EQ(clusters.size(), 4u);
    CY_CHECK_EQ(grass.size(), 4u);
    CY_CHECK_EQ(streaming.clusters_of(first).size(), 2u);
    CY_CHECK_EQ(loader_calls, 2u);

    // A cluster knows which cell it streamed with.
    const FoliageCluster* bound = clusters.find(streaming.clusters_of(first)[0]);
    CY_REQUIRE(bound != nullptr);
    CY_CHECK_EQ(bound->cell().value, first.value);

    // The region unloads and its foliage goes with it — which falls out of consuming the same
    // events the world already emits rather than out of a second notification model.
    CY_REQUIRE(events.emit(event_for(first, cy::world::CellEventKind::Evicted, everything, 3))
                   .has_value());
    auto evicted = streaming.tick();
    CY_REQUIRE(evicted.has_value());
    CY_CHECK_EQ(evicted.value().cells_released, 1u);
    CY_CHECK_EQ(evicted.value().clusters_evicted, 2u);
    CY_CHECK_EQ(clusters.size(), 2u);
    CY_CHECK_EQ(grass.size(), 2u);
    CY_CHECK_EQ(streaming.clusters_of(first).size(), 0u);
    // The other cell's clusters are untouched.
    CY_CHECK_EQ(streaming.clusters_of(second).size(), 2u);
}

CY_TEST_CASE("a cluster is independently evictable without its cell going anywhere") {
    ClusterStore clusters(test::allocator());
    GrassField grass(test::allocator());
    FoliageStreaming streaming(test::allocator(), clusters, grass, test::partition());
    cy::world::CellEventQueue events(test::allocator());
    CY_REQUIRE(streaming.attach(events, 40).has_value());
    cy::u32 loader_calls = 0;
    streaming.set_loader(&loader, &loader_calls);

    const cy::world::CellId cell =
        cy::world::cell_id_of(test::partition(), cy::world::CellCoord{3, 0, 2, 0});
    CY_REQUIRE(events
                   .emit(event_for(cell, cy::world::CellEventKind::Resident,
                                   cy::world::ChannelMask::all(), 1))
                   .has_value());
    CY_REQUIRE(streaming.tick().has_value());
    CY_REQUIRE_EQ(clusters.size(), 2u);

    // Memory pressure takes one cluster. The cell stays bound, which is what makes this a lever
    // rather than "unload the region".
    const ClusterId dropped = streaming.clusters_of(cell)[0];
    CY_CHECK(streaming.drop_cluster(dropped));
    CY_CHECK_EQ(clusters.size(), 1u);
    CY_CHECK_EQ(streaming.bound_cells(), 1u);
    CY_CHECK_FALSE(streaming.drop_cluster(dropped));

    // And the cell's eviction still releases what is left, without double-evicting the one that is
    // already gone.
    CY_REQUIRE(events
                   .emit(event_for(cell, cy::world::CellEventKind::Evicted,
                                   cy::world::ChannelMask::all(), 2))
                   .has_value());
    auto report = streaming.tick();
    CY_REQUIRE(report.has_value());
    CY_CHECK_EQ(report.value().clusters_evicted, 1u);
    CY_CHECK_EQ(clusters.size(), 0u);
}

CY_TEST_CASE("a source that does not require the foliage channel streams none") {
    ClusterStore clusters(test::allocator());
    GrassField grass(test::allocator());
    FoliageStreaming streaming(test::allocator(), clusters, grass, test::partition());
    cy::world::CellEventQueue events(test::allocator());
    CY_REQUIRE(streaming.attach(events, 40).has_value());
    cy::u32 loader_calls = 0;
    streaming.set_loader(&loader, &loader_calls);

    cy::world::ChannelMask spectator;
    spectator.set(cy::world::Channel::Geometry);
    spectator.set(cy::world::Channel::Textures);
    const cy::world::CellId cell =
        cy::world::cell_id_of(test::partition(), cy::world::CellCoord{9, 0, 9, 0});
    CY_REQUIRE(
        events.emit(event_for(cell, cy::world::CellEventKind::Resident, spectator, 1)).has_value());
    auto report = streaming.tick();
    CY_REQUIRE(report.has_value());
    CY_CHECK_EQ(report.value().cells_without_channel, 1u);
    CY_CHECK_EQ(report.value().cells_bound, 0u);
    CY_CHECK_EQ(clusters.size(), 0u);
    CY_CHECK_EQ(loader_calls, 0u);
}

CY_TEST_CASE("a dedicated server keeps the foliage channel, because a felled tree is its state") {
    // `foliage` makes an instance promotable to an entity, and a felled tree is authoritative
    // state. A server that dropped the clusters could not derive the identity a promotion is
    // anchored to — see src/world/src/cell.cpp's `profile_channels()`.
    const cy::world::ChannelMask server =
        cy::world::profile_channels(cy::world::WorldProfile::DedicatedServer);
    CY_CHECK(server.has(cy::world::Channel::Foliage));
    CY_CHECK(server.has(cy::world::Channel::Terrain));
    CY_CHECK_FALSE(server.has(cy::world::Channel::Geometry));

    const cy::world::ChannelMask client =
        cy::world::profile_channels(cy::world::WorldProfile::Client);
    CY_CHECK(client.has(cy::world::Channel::Foliage));
    // The mask has a name for the channel, which is what a diagnostic prints.
    CY_CHECK_EQ(cy::world::channel_name(cy::world::Channel::Foliage), "foliage");
}

CY_TEST_CASE("streaming refuses to tick before it has joined the world's queue") {
    ClusterStore clusters(test::allocator());
    GrassField grass(test::allocator());
    FoliageStreaming streaming(test::allocator(), clusters, grass, test::partition());
    CY_CHECK_FALSE(streaming.attached());
    // Foliage consumes the world's own cell events rather than observing cells, so there is
    // genuinely nothing to tick — and saying so is better than returning an empty report that
    // reads like a world with no foliage in it.
    CY_CHECK_FALSE(streaming.tick().has_value());
}

CY_TEST_CASE("a cell the loader has nothing for is bound once, not asked again every frame") {
    ClusterStore clusters(test::allocator());
    GrassField grass(test::allocator());
    FoliageStreaming streaming(test::allocator(), clusters, grass, test::partition());
    cy::world::CellEventQueue events(test::allocator());
    CY_REQUIRE(streaming.attach(events, 40).has_value());

    cy::u32 calls = 0;
    streaming.set_loader(
        [](void* user, cy::world::CellId, const cy::world::PartitionConfig&,
           cy::Array<FoliagePopulationHandle>&) noexcept -> cy::Status {
            ++*static_cast<cy::u32*>(user);
            return cy::fail(cy::ErrorCode::NotFound, "no foliage in this cell");
        },
        &calls);

    const cy::world::CellId cell =
        cy::world::cell_id_of(test::partition(), cy::world::CellCoord{4, 0, 4, 0});
    for (cy::u64 sequence = 1; sequence <= 3; ++sequence) {
        CY_REQUIRE(events
                       .emit(event_for(cell, cy::world::CellEventKind::ChannelsChanged,
                                       cy::world::ChannelMask::all(), sequence))
                       .has_value());
    }
    auto report = streaming.tick();
    CY_REQUIRE(report.has_value());
    CY_CHECK_EQ(report.value().cells_empty, 1u);
    CY_CHECK_EQ(calls, 1u);
    CY_CHECK_EQ(streaming.bound_cells(), 1u);
    CY_CHECK_EQ(clusters.size(), 0u);
}
