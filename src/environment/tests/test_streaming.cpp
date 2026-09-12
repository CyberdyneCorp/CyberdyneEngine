// Residency levels and streaming, against `world-partition-and-streaming`'s existing cells and
// `residency`'s existing policy. Task 1.3.
//
// Nothing here constructs a parallel mechanism and every case is written to show it: the events are
// `world::CellEvent`s on a real `world::CellEventQueue`, the decisions are a real
// `residency::ResidencyServer`'s, and the only thing `FieldStreaming` contributes is the mapping
// between a cell and the tiles over it.
//
// HOW THE BINDING CASE WAS SHOWN TO BE ABLE TO FAIL: `bind_cell()`'s skip of levels declared
// `resident_everywhere` was removed, and "a cell arriving with the field channel brings its tiles"
// went red at `tiles_requested == 17` with 18 — a cell re-requesting the macro level that is
// guaranteed everywhere. It was then restored. The guaranteed-tile refusal itself is held in
// `test_store.cpp`, where deleting `FieldStore::evict_tile()`'s guard turns the eviction of a
// guaranteed tile from a refusal into a success.

#include <cy/test/test.h>

#include <cy/environment/streaming.h>

#include "fixtures.h"

using cy::environment::FieldRegistry;
using cy::environment::FieldResidency;
using cy::environment::FieldStore;
using cy::environment::FieldStreaming;
using cy::environment::FieldStreamingReport;
using cy::environment::FieldValue;
using cy::environment::ProducerKind;
using cy::environment::ProducerToken;
using cy::environment::TileAddress;
namespace test = cy::environment::test;

namespace {

constexpr cy::u64 kBigBudget = 64ULL * 1024 * 1024;

[[nodiscard]] cy::residency::SubsystemPolicy world_cells_policy(cy::u64 budget) noexcept {
    cy::residency::SubsystemPolicy policy;
    policy.domain = cy::MemoryDomain::World;
    policy.budget_bytes = budget;
    policy.budget_kind = cy::BudgetKind::Hard;
    // Zero, because the anti-oscillation guard is `residency`'s own behaviour and has its own suite
    // there; a test of field streaming that had to advance two frames to see a tile arrive would be
    // measuring that guard instead of this module.
    policy.min_residency_frames = 0;
    policy.default_cost = cy::residency::CostClass::Streamed;
    return policy;
}

[[nodiscard]] cy::world::CellEvent cell_event(cy::world::CellEventKind kind, cy::world::CellId cell,
                                              bool with_fields) noexcept {
    cy::world::CellEvent event;
    event.kind = kind;
    event.cell = cell;
    event.channels = cy::world::ChannelMask::of(cy::world::Channel::Entities);
    if (with_fields) {
        event.channels.set(cy::world::Channel::Fields);
    }
    return event;
}

}  // namespace

CY_TEST_CASE("a cell's footprint is the tiles over it, and the neighbour's row is not") {
    // A level-0 cell is 128 m; a local moisture cell is 2 m, so a tile is 32 m and a cell is four
    // tiles across. The cell's far edge belongs to its neighbour.
    const cy::world::PartitionConfig partition = test::partition();
    const auto moisture = test::moisture_like();
    cy::Array<TileAddress> tiles(test::allocator());
    CY_REQUIRE(cy::environment::cell_tile_footprint(partition, cy::world::CellCoord{0, 0, 0, 0},
                                                    moisture, moisture.id(), 0, tiles)
                   .has_value());
    CY_CHECK_EQ(tiles.size(), 16u);
    for (const TileAddress& address : tiles.span()) {
        CY_CHECK_GE(address.x, 0);
        CY_CHECK_LE(address.x, 3);
        CY_CHECK_GE(address.z, 0);
        CY_CHECK_LE(address.z, 3);
    }

    cy::Array<TileAddress> neighbour(test::allocator());
    CY_REQUIRE(cy::environment::cell_tile_footprint(partition, cy::world::CellCoord{1, 0, 0, 0},
                                                    moisture, moisture.id(), 0, neighbour)
                   .has_value());
    CY_CHECK_EQ(neighbour.size(), 16u);
    for (const TileAddress& address : neighbour.span()) {
        CY_CHECK_GE(address.x, 4);
        CY_CHECK_LE(address.x, 7);
    }

    // A cell west of the origin is four tiles too, at negative coordinates — the case truncating
    // division gets wrong by folding cell -1 onto cell 0.
    cy::Array<TileAddress> west(test::allocator());
    CY_REQUIRE(cy::environment::cell_tile_footprint(partition, cy::world::CellCoord{-1, 0, 0, 0},
                                                    moisture, moisture.id(), 0, west)
                   .has_value());
    CY_CHECK_EQ(west.size(), 16u);
    for (const TileAddress& address : west.span()) {
        CY_CHECK_GE(address.x, -4);
        CY_CHECK_LE(address.x, -1);
    }
}

CY_TEST_CASE("a page identifier distinguishes every tile, and says it is a field's") {
    const auto field = cy::environment::field_id("test.moisture");
    const cy::u64 first = cy::environment::page_of_tile(3, test::tile_at(field, 0, -5, 9));
    const cy::u64 same = cy::environment::page_of_tile(3, test::tile_at(field, 0, -5, 9));
    CY_CHECK_EQ(first, same);
    CY_CHECK_NE(first & cy::environment::kFieldPageBit, 0u);
    // Bit 55 is the split this module shares with `src/world/`'s own future pages, and nothing else
    // in the identifier may reach it.
    CY_CHECK_EQ(first >> 56U, 0u);

    CY_CHECK_NE(first, cy::environment::page_of_tile(4, test::tile_at(field, 0, -5, 9)));
    CY_CHECK_NE(first, cy::environment::page_of_tile(3, test::tile_at(field, 1, -5, 9)));
    CY_CHECK_NE(first, cy::environment::page_of_tile(3, test::tile_at(field, 0, -6, 9)));
    CY_CHECK_NE(first, cy::environment::page_of_tile(3, test::tile_at(field, 0, -5, 10)));
    CY_CHECK_NE(first, cy::environment::page_of_tile(
                           3, test::tile_at(field, 0, -5, 9, cy::environment::FieldLayer::Delta)));
}

CY_TEST_CASE("the streamer refuses to attach when nobody has declared the budget") {
    // "This module does not register the residency subsystem": the budget belongs to whoever owns
    // the world, and a module that registered one would be deciding it for them.
    FieldRegistry registry(test::allocator());
    const cy::world::PartitionConfig partition = test::partition();
    FieldStore store(test::allocator(), registry, partition);
    cy::residency::ResidencyServer server(test::allocator());
    cy::world::CellEventQueue events(test::allocator());
    FieldStreaming streaming(test::allocator(), store, server);

    const cy::Status refused = streaming.attach(events, 10);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(refused.error().code == cy::ErrorCode::Unavailable);

    CY_REQUIRE(server
                   .register_subsystem(cy::residency::Subsystem::WorldCells,
                                       world_cells_policy(kBigBudget))
                   .has_value());
    CY_CHECK(streaming.attach(events, 10).has_value());
}

CY_TEST_CASE("a cell arriving with the field channel brings its tiles, and leaving stops asking") {
    FieldRegistry registry(test::allocator());
    const cy::world::PartitionConfig partition = test::partition();
    FieldStore store(test::allocator(), registry, partition);
    const auto moisture = test::moisture_like();
    CY_REQUIRE(registry.declare(moisture).has_value());
    cy::Expected<ProducerToken, cy::Error> token =
        registry.claim(moisture.id(), "cook.moisture", ProducerKind::Baked);
    CY_REQUIRE(token.has_value());

    cy::residency::ResidencyServer server(test::allocator());
    CY_REQUIRE(server
                   .register_subsystem(cy::residency::Subsystem::WorldCells,
                                       world_cells_policy(kBigBudget))
                   .has_value());
    cy::world::CellEventQueue events(test::allocator());
    FieldStreaming streaming(test::allocator(), store, server);
    CY_REQUIRE(streaming.adopt(std::move(*token), FieldResidency::Local).has_value());
    CY_REQUIRE(streaming.attach(events, 10).has_value());

    const cy::world::CellCoord coord{0, 0, 0, 0};
    const cy::world::CellId cell = cy::world::cell_id_of(partition, coord);
    CY_REQUIRE(streaming.declare_cell(cell, coord).has_value());

    // A cell with no field channel binds nothing: there is no field data to stream for it, and
    // asking the policy for tiles nothing can load is how a budget gets spent on nothing.
    CY_REQUIRE(
        events.emit(cell_event(cy::world::CellEventKind::Resident, cell, false)).has_value());
    cy::Expected<FieldStreamingReport, cy::Error> quiet = streaming.tick(0.0);
    CY_REQUIRE(quiet.has_value());
    CY_CHECK_EQ(quiet->cells_bound, 0u);
    CY_CHECK_EQ(quiet->tiles_requested, 0u);

    CY_REQUIRE(events.emit(cell_event(cy::world::CellEventKind::Resident, cell, true)).has_value());
    cy::Expected<FieldStreamingReport, cy::Error> first = streaming.tick(0.0);
    CY_REQUIRE(first.has_value());
    CY_CHECK_EQ(first->cells_bound, 1u);
    // Local (16 tiles) and regional (8 m cells, so a 128 m tile: one tile). The macro level is
    // declared resident everywhere and is not requested per cell.
    CY_CHECK_EQ(first->tiles_requested, 17u);
    CY_CHECK_EQ(first->tiles_loaded, 17u);
    CY_CHECK(store.is_resident(test::tile_at(moisture.id(), 0, 0, 0)));
    CY_CHECK(store.is_resident(test::tile_at(moisture.id(), 1, 0, 0)));
    CY_CHECK_FALSE(store.is_resident(test::tile_at(moisture.id(), 0, 4, 0)));

    // A second tick asks for nothing: the tiles are resident and a request for a resident tile is a
    // request the policy would only have to deduplicate.
    cy::Expected<FieldStreamingReport, cy::Error> idle = streaming.tick(1.0);
    CY_REQUIRE(idle.has_value());
    CY_CHECK_EQ(idle->tiles_requested, 0u);

    CY_REQUIRE(events.emit(cell_event(cy::world::CellEventKind::Evicted, cell, true)).has_value());
    cy::Expected<FieldStreamingReport, cy::Error> gone = streaming.tick(2.0);
    CY_REQUIRE(gone.has_value());
    CY_CHECK_EQ(gone->cells_released, 1u);
    CY_CHECK_EQ(streaming.bound_cells(), 0u);
}

CY_TEST_CASE("a cooked source fills the tiles the policy admits") {
    // The seam every producer row plugs into: the substrate knows when a tile is wanted and where
    // to put it, and the row knows what is in it.
    FieldRegistry registry(test::allocator());
    const cy::world::PartitionConfig partition = test::partition();
    FieldStore store(test::allocator(), registry, partition);
    const auto moisture = test::moisture_like();
    CY_REQUIRE(registry.declare(moisture).has_value());
    cy::Expected<ProducerToken, cy::Error> token =
        registry.claim(moisture.id(), "cook.moisture", ProducerKind::Baked);
    CY_REQUIRE(token.has_value());

    cy::residency::ResidencyServer server(test::allocator());
    CY_REQUIRE(server
                   .register_subsystem(cy::residency::Subsystem::WorldCells,
                                       world_cells_policy(kBigBudget))
                   .has_value());
    cy::world::CellEventQueue events(test::allocator());
    FieldStreaming streaming(test::allocator(), store, server);
    CY_REQUIRE(streaming.adopt(std::move(*token), FieldResidency::Local).has_value());

    struct Cook {
        cy::u32 loads = 0;
    };
    Cook cook;
    const auto loader = [](void* user, const TileAddress& address,
                           cy::Array<cy::u8>& out) noexcept -> cy::Status {
        static_cast<Cook*>(user)->loads += 1;
        // A tile whose every byte is the tile's own x coordinate: enough to tell a cooked tile from
        // a defaulted one, and from its neighbour.
        const cy::u32 bytes = 16U * 16U;
        for (cy::u32 index = 0; index < bytes; ++index) {
            if (cy::Status pushed = out.push_back(static_cast<cy::u8>(64 + address.x)); !pushed) {
                return pushed;
            }
        }
        return cy::ok();
    };
    CY_REQUIRE(streaming.set_loader(moisture.id(), loader, &cook).has_value());
    CY_REQUIRE(streaming.attach(events, 10).has_value());

    const cy::world::CellCoord coord{0, 0, 0, 0};
    const cy::world::CellId cell = cy::world::cell_id_of(partition, coord);
    CY_REQUIRE(streaming.declare_cell(cell, coord).has_value());
    CY_REQUIRE(events.emit(cell_event(cy::world::CellEventKind::Resident, cell, true)).has_value());

    cy::Expected<FieldStreamingReport, cy::Error> report = streaming.tick(0.0);
    CY_REQUIRE(report.has_value());
    CY_CHECK_EQ(report->tiles_loaded, 17u);
    CY_CHECK_EQ(cook.loads, 17u);

    // The cooked bytes are what a sample reads: tile 0 carries 64/255 and tile 1 carries 65/255.
    CY_CHECK_NEAR(
        store.sample_at(moisture.id(), test::at(4.0, 4.0), FieldResidency::Local).value.x(),
        64.0F / 255.0F, 1.0F / 255.0F);
    CY_CHECK_NEAR(
        store.sample_at(moisture.id(), test::at(40.0, 4.0), FieldResidency::Local).value.x(),
        65.0F / 255.0F, 1.0F / 255.0F);
}

CY_TEST_CASE("a budget may not evict the level that must exist everywhere") {
    // "WHEN a region is far from any viewer, THEN its macro field values SHALL still exist and
    // evolve, without fine tiles being resident."
    FieldRegistry registry(test::allocator());
    const cy::world::PartitionConfig partition = test::partition();
    FieldStore store(test::allocator(), registry, partition);
    const auto moisture = test::moisture_like();
    CY_REQUIRE(registry.declare(moisture).has_value());
    cy::Expected<ProducerToken, cy::Error> token =
        registry.claim(moisture.id(), "cook.moisture", ProducerKind::Baked);
    CY_REQUIRE(token.has_value());

    cy::residency::ResidencyServer server(test::allocator());
    // Room for twenty tiles: the sixteen guaranteed macro ones and four more. The cell below wants
    // seventeen fine tiles on top of that, so the budget is what decides how many of them survive —
    // and the sixteen are not among the candidates.
    CY_REQUIRE(server
                   .register_subsystem(cy::residency::Subsystem::WorldCells,
                                       world_cells_policy(20U * FieldStore::tile_bytes(moisture)))
                   .has_value());
    cy::world::CellEventQueue events(test::allocator());
    FieldStreaming streaming(test::allocator(), store, server);
    CY_REQUIRE(streaming.adopt(std::move(*token), FieldResidency::Local).has_value());
    CY_REQUIRE(streaming.attach(events, 10).has_value());

    // The macro level over a 4x4 tile rectangle — sixteen tiles, none of which the budget above
    // could afford if they went through it. They do not: a guaranteed tile is the coarse fallback
    // every other answer is defined in terms of.
    CY_REQUIRE(streaming.guarantee_macro(moisture.id(), 0, 0, 3, 3, 0.0).has_value());
    CY_CHECK_EQ(store.tile_count(), 16u);
    CY_CHECK(store.sample_deterministic(moisture.id(), test::at(2'000.0, 2'000.0)).resolved);

    const cy::world::CellCoord coord{0, 0, 0, 0};
    const cy::world::CellId cell = cy::world::cell_id_of(partition, coord);
    CY_REQUIRE(streaming.declare_cell(cell, coord).has_value());
    CY_REQUIRE(events.emit(cell_event(cy::world::CellEventKind::Resident, cell, true)).has_value());

    // Several ticks: the fine tiles come and go under the budget while the macro level does not
    // move, whatever the policy decides.
    cy::u32 refused = 0;
    for (cy::u32 frame = 0; frame < 6; ++frame) {
        cy::Expected<FieldStreamingReport, cy::Error> report =
            streaming.tick(static_cast<cy::f64>(frame));
        CY_REQUIRE(report.has_value());
        refused += report->evictions_refused;
        server.end_frame(static_cast<cy::f64>(frame));
    }

    for (cy::i32 z = 0; z <= 3; ++z) {
        for (cy::i32 x = 0; x <= 3; ++x) {
            CY_CHECK(store.is_resident(test::tile_at(moisture.id(), 2, x, z)));
        }
    }
    // The deterministic answer at an unloaded position is still there, which is the point of the
    // whole arrangement rather than a property of this test's numbers.
    CY_CHECK(store.sample_deterministic(moisture.id(), test::at(2'000.0, 2'000.0)).resolved);
    CY_TEST_MESSAGE("evictions the policy ordered for guaranteed tiles, refused: ", refused);
}

CY_TEST_CASE("a tile nobody wants is given back to the policy") {
    FieldRegistry registry(test::allocator());
    const cy::world::PartitionConfig partition = test::partition();
    FieldStore store(test::allocator(), registry, partition);
    const auto moisture = test::moisture_like();
    CY_REQUIRE(registry.declare(moisture).has_value());
    cy::Expected<ProducerToken, cy::Error> token =
        registry.claim(moisture.id(), "cook.moisture", ProducerKind::Baked);
    CY_REQUIRE(token.has_value());

    cy::residency::ResidencyServer server(test::allocator());
    // Room for eight tiles; two cells want thirty-four between them.
    CY_REQUIRE(server
                   .register_subsystem(cy::residency::Subsystem::WorldCells,
                                       world_cells_policy(8U * FieldStore::tile_bytes(moisture)))
                   .has_value());
    cy::world::CellEventQueue events(test::allocator());
    FieldStreaming streaming(test::allocator(), store, server);
    CY_REQUIRE(streaming.adopt(std::move(*token), FieldResidency::Local).has_value());
    CY_REQUIRE(streaming.attach(events, 10).has_value());

    for (cy::i32 index = 0; index < 2; ++index) {
        const cy::world::CellCoord coord{index, 0, 0, 0};
        const cy::world::CellId cell = cy::world::cell_id_of(partition, coord);
        CY_REQUIRE(streaming.declare_cell(cell, coord).has_value());
        CY_REQUIRE(
            events.emit(cell_event(cy::world::CellEventKind::Resident, cell, true)).has_value());
    }

    cy::u32 evicted = 0;
    for (cy::u32 frame = 0; frame < 8; ++frame) {
        cy::Expected<FieldStreamingReport, cy::Error> report =
            streaming.tick(static_cast<cy::f64>(frame));
        CY_REQUIRE(report.has_value());
        evicted += report->tiles_evicted;
        server.end_frame(static_cast<cy::f64>(frame));
    }

    // The budget is what bounds residency, and it is `residency`'s budget rather than one this
    // module keeps: no tile count here is asserted against a number this module chose.
    CY_CHECK_LE(store.bytes_resident(),
                server.stats(cy::residency::Subsystem::WorldCells).budget_bytes);
    CY_TEST_MESSAGE("tiles the policy took back: ", evicted,
                    ", resident bytes: ", store.bytes_resident());
}
