// The translation between the world's persistence overlay and a save's. src/world/persistence/.
//
// THE ROUND TRIP GOES THROUGH A REAL SAVE. Each case that says "save" commits to a `SaveArchive`
// over a `MemoryBackend` and loads back out of it, so what is compared is what an encoded,
// hash-verified generation gives back — not what a translation into memory and straight back out
// again would give, which would pass for a pair of functions that agreed on a wrong encoding.
//
// TWO ANSWERS ARE COMPARED IN EVERY CASE THAT CLAIMS A RESULT: the overlay that went in and the
// overlay that came out, by `same_overlay()` below, cell by cell, record by record and byte by
// byte. `m11a:save-has-an-engine-consumer` runs the first two cases by name.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/attributes.h>
#include <cy/core/reflect/type_info.h>
#include <cy/ecs/component.h>
#include <cy/save/archive.h>
#include <cy/save/storage.h>
#include <cy/world/overlay.h>
#include <cy/world/persistence/save_translation.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace {

using namespace cy;

[[nodiscard]] Allocator& memory() noexcept {
    return system_allocator(MemoryDomain::World);
}

[[nodiscard]] reflect::FieldInfo make_field(const char* name, u32 id, u32 offset,
                                            reflect::PersistenceKind persistence) noexcept {
    reflect::FieldInfo field;
    field.name = name;
    field.id = reflect::FieldId(id);
    field.kind = reflect::FieldKind::U32;
    field.offset = offset;
    field.size = sizeof(u32);
    field.attributes.declared = reflect::AttributeKind::Persistence;
    field.attributes.persistence = persistence;
    return field;
}

// Identifiers in the 9700s: visibly not ones identity/manifest.toml issued, and clear of the other
// suites' ranges (src/world/tests/fixtures.h, src/save/tests/fixtures.h).

/// Everything a save carries: the shape a component meant for the overlay has.
struct Structure {
    u32 material = 0;
    u32 integrity = 0;
};

/// One persistent field and one derived one.
struct Door {
    u32 open = 0;
    u32 swing_cache = 0;  ///< Derived: reconstructed after load, never saved.
};

[[nodiscard]] const reflect::TypeInfo& structure_type() noexcept {
    using reflect::PersistenceKind;
    static const reflect::FieldInfo fields[] = {
        make_field("material", 1, static_cast<u32>(offsetof(Structure, material)),
                   PersistenceKind::PersistentState),
        make_field("integrity", 2, static_cast<u32>(offsetof(Structure, integrity)),
                   PersistenceKind::PersistentState),
    };
    static reflect::TypeInfo info;
    info.name = "cy::world::test::Structure";
    info.id = reflect::TypeId(9701);
    info.size = static_cast<u32>(sizeof(Structure));
    info.alignment = static_cast<u32>(alignof(Structure));
    info.trivially_relocatable = true;
    info.fields = fields;
    info.field_count = 2;
    return info;
}

[[nodiscard]] const reflect::TypeInfo& door_type() noexcept {
    using reflect::PersistenceKind;
    static const reflect::FieldInfo fields[] = {
        make_field("open", 1, static_cast<u32>(offsetof(Door, open)),
                   PersistenceKind::PersistentState),
        make_field("swing_cache", 2, static_cast<u32>(offsetof(Door, swing_cache)),
                   PersistenceKind::Derived),
    };
    static reflect::TypeInfo info;
    info.name = "cy::world::test::Door";
    info.id = reflect::TypeId(9702);
    info.size = static_cast<u32>(sizeof(Door));
    info.alignment = static_cast<u32>(alignof(Door));
    info.trivially_relocatable = true;
    info.fields = fields;
    info.field_count = 2;
    return info;
}

/// A world's component registry with both types in it, and the translation reading it.
struct Registry {
    ecs::ComponentRegistry components{memory()};
    ecs::ComponentTypeId structure = ecs::kInvalidComponent;
    ecs::ComponentTypeId door = ecs::kInvalidComponent;

    Registry() {
        structure = *components.register_reflected(structure_type());
        door = *components.register_reflected(door_type());
    }

    [[nodiscard]] world::SaveTranslation translation() const noexcept {
        world::SaveTranslation out;
        out.components = &components;
        return out;
    }
};

template <class T>
[[nodiscard]] Span<const u8> bytes_of(const T& value) noexcept {
    return Span<const u8>{reinterpret_cast<const u8*>(&value), sizeof(T)};
}

[[nodiscard]] bool contains(Span<const world::PersistentId> ids, world::PersistentId id) noexcept {
    return std::ranges::any_of(ids,
                               [id](world::PersistentId candidate) { return candidate == id; });
}

/// Every removal and override `left` holds for one cell, present in `right` with the same bytes.
[[nodiscard]] bool cell_contained_in(const world::PersistenceOverlay& left,
                                     const world::PersistenceOverlay& right,
                                     world::CellId cell) noexcept {
    const world::CellOverlay* mine = left.find(cell);
    const world::CellOverlay* theirs = right.find(cell);
    if (mine == nullptr || theirs == nullptr) {
        return false;
    }
    for (const world::PersistentId removed : mine->removed.span()) {
        if (!contains(theirs->removed.span(), removed)) {
            return false;
        }
    }
    for (const world::ComponentOverride& change : mine->overrides.span()) {
        const Span<const u8> a = left.component_override(cell, change.entity, change.component);
        const Span<const u8> b = right.component_override(cell, change.entity, change.component);
        if (a.size() != b.size() || std::memcmp(a.data(), b.data(), a.size()) != 0) {
            return false;
        }
    }
    return mine->removed.size() == theirs->removed.size() &&
           mine->overrides.size() == theirs->overrides.size();
}

/// The two answers every result-claiming case compares.
[[nodiscard]] bool same_overlay(const world::PersistenceOverlay& left,
                                const world::PersistenceOverlay& right) noexcept {
    Array<world::CellId> cells(memory());
    Array<world::CellId> others(memory());
    if (!left.cells(cells) || !right.cells(others) || cells.size() != others.size()) {
        return false;
    }
    for (usize index = 0; index < cells.size(); ++index) {
        if (!(cells[index] == others[index]) || !cell_contained_in(left, right, cells[index])) {
            return false;
        }
    }
    return true;
}

/// A city block: three cells, removals in two, structural damage in all three, and a door.
void populate(const Registry& registry, world::PersistenceOverlay& overlay) {
    const world::CellId north{0x1001};
    const world::CellId south{0x2002};
    const world::CellId harbour{0x3003};
    CY_REQUIRE(overlay.record_removed(north, world::PersistentId{11}));
    CY_REQUIRE(overlay.record_removed(harbour, world::PersistentId{31}));
    CY_REQUIRE(overlay.record_component(north, world::PersistentId{12}, registry.structure,
                                        bytes_of(Structure{4, 60})));
    CY_REQUIRE(overlay.record_component(south, world::PersistentId{21}, registry.structure,
                                        bytes_of(Structure{7, 15})));
    CY_REQUIRE(overlay.record_component(harbour, world::PersistentId{32}, registry.structure,
                                        bytes_of(Structure{2, 99})));
    CY_REQUIRE(overlay.record_component(harbour, world::PersistentId{32}, registry.door,
                                        bytes_of(Door{1, 0})));
}

/// World → save overlay → committed generation → loaded save overlay → world.
void save_and_load(const Registry& registry, const world::PersistenceOverlay& original,
                   world::PersistenceOverlay& loaded, world::TranslationReport& saved_counts,
                   world::TranslationReport& loaded_counts) {
    save::Overlay outgoing(memory());
    CY_REQUIRE(world::to_save_overlay(original, registry.translation(), outgoing, &saved_counts));

    save::MemoryBackend store(memory());
    save::SaveArchive archive(memory());
    CY_REQUIRE(archive.open(store));
    save::SaveIdentity identity;
    identity.build_id = "world-persistence-test";
    CY_REQUIRE(archive.commit(outgoing, identity));

    save::Overlay incoming(memory());
    save::LoadReport report;
    CY_REQUIRE(archive.load(save::LoadPolicy{}, incoming, report));
    CY_REQUIRE(world::from_save_overlay(incoming, registry.translation(), loaded, &loaded_counts));
}

}  // namespace

CY_TEST_CASE("a world overlay survives a save and a load unchanged") {
    const Registry registry;
    world::PersistenceOverlay original(memory());
    populate(registry, original);

    world::PersistenceOverlay loaded(memory());
    world::TranslationReport saved_counts;
    world::TranslationReport loaded_counts;
    save_and_load(registry, original, loaded, saved_counts, loaded_counts);

    CY_CHECK(same_overlay(original, loaded));
    CY_CHECK(same_overlay(loaded, original));
    CY_CHECK_EQ(saved_counts.regions, 3U);
    CY_CHECK_EQ(saved_counts.removals, 2U);
    CY_CHECK_EQ(saved_counts.overrides, 4U);
    CY_CHECK_EQ(loaded_counts.regions, saved_counts.regions);
    CY_CHECK_EQ(loaded_counts.removals, saved_counts.removals);
    CY_CHECK_EQ(loaded_counts.overrides, saved_counts.overrides);
    CY_CHECK_EQ(loaded_counts.unknown_types, 0U);
}

CY_TEST_CASE("a changed persistent field is detected after the round trip") {
    // The comparison above could pass for a `same_overlay` that answered true for everything, and
    // for a translation that loaded whatever was in the first save. This case changes ONE field of
    // ONE component and requires both halves to see it: the comparison tells the two worlds apart,
    // and a second save carries the new value rather than the old one.
    const Registry registry;
    world::PersistenceOverlay original(memory());
    populate(registry, original);

    world::PersistenceOverlay changed(memory());
    populate(registry, changed);
    CY_REQUIRE(changed.record_component(world::CellId{0x2002}, world::PersistentId{21},
                                        registry.structure, bytes_of(Structure{7, 14})));
    CY_CHECK_FALSE(same_overlay(original, changed));

    world::PersistenceOverlay loaded(memory());
    world::TranslationReport saved_counts;
    world::TranslationReport loaded_counts;
    save_and_load(registry, changed, loaded, saved_counts, loaded_counts);

    CY_CHECK(same_overlay(changed, loaded));
    CY_CHECK_FALSE(same_overlay(original, loaded));
    const Span<const u8> bytes = loaded.component_override(
        world::CellId{0x2002}, world::PersistentId{21}, registry.structure);
    CY_REQUIRE_EQ(bytes.size(), sizeof(Structure));
    Structure read;
    std::memcpy(&read, bytes.data(), sizeof(read));
    CY_CHECK_EQ(read.material, 7U);
    CY_CHECK_EQ(read.integrity, 14U);
}

CY_TEST_CASE("a removed entity's overrides do not reach the save") {
    // The world overlay keeps an override recorded before the entity was destroyed; the save model
    // makes a tombstone the whole delta and refuses a write to one. The translation has to drop
    // the dead fields rather than fail the save over them.
    const Registry registry;
    world::PersistenceOverlay overlay(memory());
    const world::CellId cell{0x4004};
    CY_REQUIRE(overlay.record_component(cell, world::PersistentId{41}, registry.structure,
                                        bytes_of(Structure{3, 1})));
    CY_REQUIRE(overlay.record_removed(cell, world::PersistentId{41}));

    save::Overlay out(memory());
    world::TranslationReport counts;
    CY_REQUIRE(world::to_save_overlay(overlay, registry.translation(), out, &counts));
    CY_CHECK_EQ(counts.removals, 1U);
    CY_CHECK_EQ(counts.overrides, 0U);
    const save::Entry* entry =
        out.find_entry(save::RegionKey{cell.value}, save::PersistentId{0, 41});
    CY_REQUIRE(entry != nullptr);
    CY_CHECK(entry->kind == save::EntryKind::Tombstone);
    CY_CHECK(entry->components.empty());
}

CY_TEST_CASE("world state a save cannot carry is refused rather than dropped") {
    const Registry registry;
    {
        world::PersistenceOverlay overlay(memory());
        CY_REQUIRE(overlay.set_variable(7, 3));
        save::Overlay out(memory());
        const Status translated = world::to_save_overlay(overlay, registry.translation(), out);
        CY_REQUIRE_FALSE(translated.has_value());
        CY_CHECK(translated.error().code == ErrorCode::NotImplemented);
    }
    {
        world::PersistenceOverlay overlay(memory());
        const u8 blob[] = {1, 2, 3};
        CY_REQUIRE(overlay.record_blob(world::CellId{0x5005}, 1, 1, Span<const u8>{blob, 3}));
        save::Overlay out(memory());
        const Status translated = world::to_save_overlay(overlay, registry.translation(), out);
        CY_REQUIRE_FALSE(translated.has_value());
        CY_CHECK(translated.error().code == ErrorCode::NotImplemented);
    }
    {
        world::PersistenceOverlay overlay(memory());
        save::Overlay out(memory());
        const Status translated = world::to_save_overlay(overlay, world::SaveTranslation{}, out);
        CY_REQUIRE_FALSE(translated.has_value());
        CY_CHECK(translated.error().code == ErrorCode::InvalidArgument);
    }
}

CY_TEST_CASE("a field the save does not carry comes back at its default") {
    const Registry registry;
    world::PersistenceOverlay overlay(memory());
    const world::CellId cell{0x6006};
    CY_REQUIRE(overlay.record_component(cell, world::PersistentId{61}, registry.door,
                                        bytes_of(Door{1, 77})));

    world::PersistenceOverlay loaded(memory());
    world::TranslationReport saved_counts;
    world::TranslationReport loaded_counts;
    save_and_load(registry, overlay, loaded, saved_counts, loaded_counts);

    const Span<const u8> bytes =
        loaded.component_override(cell, world::PersistentId{61}, registry.door);
    CY_REQUIRE_EQ(bytes.size(), sizeof(Door));
    Door read;
    std::memcpy(&read, bytes.data(), sizeof(read));
    CY_CHECK_EQ(read.open, 1U);
    CY_CHECK_EQ(read.swing_cache, 0U);
}

CY_TEST_CASE("a saved record of a type this world does not register is counted, not applied") {
    const Registry registry;
    save::Overlay saved(memory());
    const Structure value{5, 5};
    CY_REQUIRE(saved.record_component(save::RegionKey{0x7007}, save::PersistentId{0, 71},
                                      structure_type(), &value, 1));

    ecs::ComponentRegistry other(memory());
    CY_REQUIRE(other.register_reflected(door_type()));
    world::SaveTranslation translation;
    translation.components = &other;

    world::PersistenceOverlay out(memory());
    world::TranslationReport counts;
    CY_REQUIRE(world::from_save_overlay(saved, translation, out, &counts));
    CY_CHECK_EQ(counts.unknown_types, 1U);
    CY_CHECK_EQ(counts.overrides, 0U);
    CY_CHECK(out.find(world::CellId{0x7007}) == nullptr);
}

CY_TEST_CASE("a saved persistent id wider than the world's is refused") {
    const Registry registry;
    save::Overlay saved(memory());
    CY_REQUIRE(saved.destroy_entity(save::RegionKey{0x8008}, save::PersistentId{1, 81}));
    world::PersistenceOverlay out(memory());
    const Status applied = world::from_save_overlay(saved, registry.translation(), out);
    CY_REQUIRE_FALSE(applied.has_value());
    CY_CHECK(applied.error().code == ErrorCode::OutOfRange);
}

CY_TEST_CASE("an autosave captures only the cells recorded since the last one") {
    // `save-and-persistence`: "an autosave ... SHALL cost work proportional to what changed, not
    // to the size of the world". The capture reads the world overlay's dirty cells and no others.
    const Registry registry;
    world::PersistenceOverlay overlay(memory());
    populate(registry, overlay);
    CY_CHECK_EQ(overlay.dirty_cell_count(), 3U);
    overlay.clear_dirty();
    CY_CHECK_EQ(overlay.dirty_cell_count(), 0U);

    CY_REQUIRE(overlay.record_component(world::CellId{0x2002}, world::PersistentId{22},
                                        registry.structure, bytes_of(Structure{1, 1})));
    CY_CHECK_EQ(overlay.dirty_cell_count(), 1U);

    world::SaveTranslation translation = registry.translation();
    translation.dirty_cells_only = true;
    save::Overlay out(memory());
    world::TranslationReport counts;
    CY_REQUIRE(world::to_save_overlay(overlay, translation, out, &counts));
    CY_CHECK_EQ(counts.regions, 1U);
    CY_CHECK_EQ(counts.overrides, 2U);
    CY_CHECK_EQ(out.region_count(), 1U);
    CY_CHECK(out.find_region(save::RegionKey{0x2002}) != nullptr);
}
