// SPDX-License-Identifier: MIT
// The large-world save benchmark. M11.e, closing `m11a:save-benchmark`.
//
// ================================================================================================
// WHAT THE REQUIREMENT ASKS FOR
// ================================================================================================
//
// `save-and-persistence` — "Save performance and testing": "the engine SHALL maintain a
// large-world save benchmark: a world of over a million persistent objects with most regions
// unloaded and tens of thousands of dirty records, producing an autosave with no world-wide load,
// no full entity scan, and a bounded main-thread cost" — and its scenario: "WHEN the large-world
// benchmark runs THEN save cost SHALL scale with changes rather than with world size".
//
// ================================================================================================
// WHAT ONE OPERATION IS
// ================================================================================================
//
// One AUTOSAVE, end to end through engine code and nothing written for the benchmark:
//
//   1. the capture — `world::to_save_overlay()` with `dirty_cells_only`, translating the world
//      overlay's dirty cells into the long-lived `save::Overlay` that already holds every other
//      region's state (src/world/persistence/);
//   2. the encoding — `save::encode_region()` for each of the save overlay's dirty regions, which
//      is exactly what `SaveArchive::append_journal` writes, into one reused buffer.
//
// The write to storage is left out on purpose: it is one `SaveBackend::write` per region, whose
// cost is the backend's, and a `MemoryBackend` journal grows by a region per call, so a body that
// wrote would measure a different store on every sample.
//
// ================================================================================================
// THE WORLD, AND WHY THERE ARE TWO OF THEM
// ================================================================================================
//
//   save/autosave-1m-objects    4 096 cells × 256 persistent objects = 1 048 576, one in sixteen
//                               of them (65 536) already holding saved state in both overlays
//   save/autosave-64k-objects   the same with 256 cells: 65 536 objects, 4 096 with saved state
//
// An unchanged authored object contributes nothing to either overlay — that is the delta model —
// so a world's size reaches the save path only as the cells and the records that have accumulated
// state, and the fixture carries both at sixteen times the scale in the first body. One in sixteen
// rather than every object because EVERY object with state was measured, at 3.2 GB resident for the
// pair: a `save::Overlay` entry costs about 3 KB today (src/save/README.md records it), which is
// a memory finding about the store and not what this benchmark defends.
//
// In both, 80 cells are "resident" and every object in them changed since the last autosave:
// 20 480 dirty records, the requirement's "tens of thousands". The other cells are unloaded — no
// cell is instantiated at all, and the save path has no route to one. Residency is not modelled
// further because nothing that writes a save reads it (src/save/README.md).
//
// THE SCENARIO IS THE PAIR. The two bodies do the same work over worlds sixteen times apart in
// size, so "save cost SHALL scale with changes rather than with world size" is the claim that their
// ratios are close — and `m11a:save-benchmark` checks that from the results file, beside each
// body's own threshold in benchmarks/baseline.json. A capture that translated the whole overlay,
// or an encoder that walked every region, makes the first sixteen times the second.
//
// ONE ITERATION IS ONE AUTOSAVE, milliseconds long, so both bodies start the runner's scaling at
// one iteration (CY_BENCHMARK_STARTING_AT) rather than at the thousand a nanosecond body needs.
//
// EVERY BODY IS STEADY STATE. Neither overlay's dirty flags are cleared, so each iteration captures
// and encodes the same 80 regions; re-recording a record into the save overlay overwrites it, so
// nothing grows between samples.

#include <cy/bench/bench.h>

#include <cy/core/memory/array.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/attributes.h>
#include <cy/core/reflect/type_info.h>
#include <cy/ecs/component.h>
#include <cy/save/container.h>
#include <cy/save/overlay.h>
#include <cy/world/overlay.h>
#include <cy/world/persistence/save_translation.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

using namespace cy;

/// Stop the run rather than report a number produced by a fixture that failed to build.
void require(bool condition, const char* what) noexcept {
    if (!condition) {
        std::fprintf(stderr, "cy_bench_save: %s failed; the measurement would be meaningless\n",
                     what);
        std::abort();
    }
}

constexpr u32 kObjectsPerCell = 256;
/// One object in this many holds state from before the autosave being measured.
constexpr u32 kSavedStride = 16;
constexpr u32 kResidentCells = 80;
/// One in sixteen objects in a resident cell is destroyed rather than modified, so the capture
/// writes tombstones as well as records.
constexpr u32 kRemovalStride = 16;

/// The persistent half of a building: the shape a component meant for the overlay has.
struct Structure {
    u32 material = 0;
    u32 integrity = 0;
};

[[nodiscard]] reflect::FieldInfo persistent_u32(const char* name, u32 id, u32 offset) noexcept {
    reflect::FieldInfo field;
    field.name = name;
    field.id = reflect::FieldId(id);
    field.kind = reflect::FieldKind::U32;
    field.offset = offset;
    field.size = sizeof(u32);
    field.attributes.declared = reflect::AttributeKind::Persistence;
    field.attributes.persistence = reflect::PersistenceKind::PersistentState;
    return field;
}

/// Hand-written for the reason src/save/tests/fixtures.h gives; 9790 is visibly not a manifest id.
[[nodiscard]] const reflect::TypeInfo& structure_type() noexcept {
    static const reflect::FieldInfo fields[] = {
        persistent_u32("material", 1, static_cast<u32>(offsetof(Structure, material))),
        persistent_u32("integrity", 2, static_cast<u32>(offsetof(Structure, integrity))),
    };
    static reflect::TypeInfo info;
    info.name = "cy::bench::save::Structure";
    info.id = reflect::TypeId(9790);
    info.size = static_cast<u32>(sizeof(Structure));
    info.alignment = static_cast<u32>(alignof(Structure));
    info.trivially_relocatable = true;
    info.fields = fields;
    info.field_count = 2;
    return info;
}

[[nodiscard]] Span<const u8> bytes_of(const Structure& value) noexcept {
    return Span<const u8>{reinterpret_cast<const u8*>(&value), sizeof(value)};
}

/// A world of `cells` × 256 persistent objects, one in sixteen with saved state in both overlays,
/// and every object in the first 80 cells changed since the last autosave.
struct World {
    Allocator& allocator = system_allocator(MemoryDomain::World);
    ecs::ComponentRegistry components{allocator};
    ecs::ComponentTypeId structure = ecs::kInvalidComponent;
    world::PersistenceOverlay overlay{allocator};
    save::Overlay store{allocator};
    world::SaveTranslation capture;
    Array<save::RegionKey> dirty{allocator};
    Array<u8> chunk{allocator};

    explicit World(u32 cells) {
        const Expected<ecs::ComponentTypeId, Error> registered =
            components.register_reflected(structure_type());
        require(registered.has_value(), "registering the component");
        structure = *registered;

        // The persistent state the last full save left: one object in sixteen, in every cell.
        for (u32 cell = 0; cell < cells; ++cell) {
            for (u32 object = 0; object < kObjectsPerCell; object += kSavedStride) {
                require(overlay
                            .record_component(cell_of(cell), id_of(cell, object), structure,
                                              bytes_of(Structure{object % 7, 100}))
                            .has_value(),
                        "recording the world's persistent state");
            }
        }
        capture.components = &components;
        require(world::to_save_overlay(overlay, capture, store).has_value(),
                "the full save the autosaves extend");
        overlay.clear_dirty();
        store.clear_dirty();

        // What the player did since: every object in the resident cells damaged or destroyed.
        for (u32 cell = 0; cell < kResidentCells; ++cell) {
            for (u32 object = 0; object < kObjectsPerCell; ++object) {
                const bool destroyed = object % kRemovalStride == 0;
                const Status changed =
                    destroyed
                        ? overlay.record_removed(cell_of(cell), id_of(cell, object))
                        : overlay.record_component(cell_of(cell), id_of(cell, object), structure,
                                                   bytes_of(Structure{object % 7, 40}));
                require(changed.has_value(), "recording a change");
            }
        }
        // Deliberately NOT asserted: that exactly the resident cells are dirty is what the
        // measurement itself shows, and a fixture that refused a world with more dirty cells would
        // hide the regression this benchmark exists to catch behind an abort.
        capture.dirty_cells_only = true;
    }

    [[nodiscard]] static world::CellId cell_of(u32 cell) noexcept {
        return world::CellId{0x1000ULL + cell};
    }
    [[nodiscard]] static world::PersistentId id_of(u32 cell, u32 object) noexcept {
        return world::PersistentId{(static_cast<u64>(cell) << 20U) + object + 1U};
    }

    /// One autosave: capture the dirty cells, then encode every dirty region.
    [[nodiscard]] usize autosave() noexcept {
        require(world::to_save_overlay(overlay, capture, store).has_value(), "the capture");
        dirty.clear();
        require(store.dirty_regions(dirty).has_value(), "listing the dirty regions");
        usize written = 0;
        for (const save::RegionKey region : dirty.span()) {
            require(save::encode_region(store, region, chunk).has_value(), "encoding a region");
            written += chunk.size();
        }
        return written;
    }
};

/// Built on first use, one at a time: the large world holds a million records in each overlay and
/// is not paid for by a run filtered to the small one.
World& large_world() {
    static World instance(4096);
    return instance;
}

World& small_world() {
    static World instance(256);
    return instance;
}

void run_autosaves(World& state, std::uint64_t iterations) {
    usize written = 0;
    for (std::uint64_t index = 0; index < iterations; ++index) {
        written += state.autosave();
    }
    CY_BENCH_KEEP(written);
}

}  // namespace

CY_BENCHMARK_STARTING_AT(
    "save/autosave-1m-objects",
    "One autosave of a world of 1 048 576 persistent objects in 4 096 cells, 65 536 "
    "of them with saved state, 80 cells resident and 20 480 dirty records: the capture "
    "through world::to_save_overlay's dirty cells and the encoding of each dirty region. "
    "`save-and-persistence`'s large-world save benchmark. A regression means the "
    "capture or the encoder started doing work per object or per region of the WORLD "
    "rather than of what changed — compare it with save/autosave-64k-objects, which must "
    "stay close to it — or the per-record encoding grew.",
    1) {
    run_autosaves(large_world(), CY_BENCH_ITERATIONS);
}

CY_BENCHMARK_STARTING_AT(
    "save/autosave-64k-objects",
    "The same autosave — 80 resident cells, 20 480 dirty records — over a world sixteen "
    "times smaller: 65 536 objects in 256 cells. The control for "
    "save/autosave-1m-objects: `save cost SHALL scale with changes rather than with "
    "world size` is the claim that the two stay close, and m11a:save-benchmark checks it. "
    "A regression in this one alone means the per-record cost of a capture or an "
    "encoding grew.",
    1) {
    run_autosaves(small_world(), CY_BENCH_ITERATIONS);
}
