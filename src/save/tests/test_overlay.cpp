// The persistence overlay: deltas, tombstones, dirty tracking, and independence from residency.
// Tasks 6.1 and 6.4.

#include <cy/save/container.h>
#include <cy/save/overlay.h>
#include <cy/test/test.h>

#include "fixtures.h"

#include <cstring>

using namespace cy;
using namespace cy::save;
using namespace cy::save::test;

namespace {

constexpr RegionKey kVillage{0x0A00'0000'0000'0001ULL};
constexpr RegionKey kQuarry{0x0A00'0000'0000'0002ULL};

Overlay make_overlay() {
    return Overlay(test_allocator());
}

/// Record one Health on `id`, the way a gameplay system would.
void record_health(Overlay& overlay, RegionKey region, PersistentId id, u32 revives) {
    Health health;
    health.revives = revives;
    health.current = 12.5F;  // RuntimeState: must not appear anywhere in the overlay
    CY_REQUIRE(overlay.record_component(region, id, health_type(), &health, 1).has_value());
}

}  // namespace

CY_TEST_CASE("an unchanged entity contributes nothing") {
    // "An entity that has not changed SHALL contribute nothing to the save."
    Overlay overlay = make_overlay();
    CY_CHECK_EQ(overlay.entry_count(), 0U);
    CY_CHECK_EQ(overlay.region_count(), 0U);
    CY_CHECK_FALSE(overlay.is_dirty());
}

CY_TEST_CASE("only the persistent field of a component reaches the overlay") {
    Overlay overlay = make_overlay();
    record_health(overlay, kVillage, entity(1), 3);

    const ComponentDelta* delta =
        overlay.find_component(kVillage, entity(1), reflect::TypeId(kHealthTypeId));
    CY_REQUIRE(delta != nullptr);
    CY_CHECK(delta->record.contains(reflect::FieldId(kHealthRevives)));
    // Authoring, RuntimeState, Derived and Transient are all absent — the classification table is
    // the only thing deciding, and there is no flag a caller could pass to get them in.
    CY_CHECK_FALSE(delta->record.contains(reflect::FieldId(kHealthMaximum)));
    CY_CHECK_FALSE(delta->record.contains(reflect::FieldId(kHealthCurrent)));
    CY_CHECK_FALSE(delta->record.contains(reflect::FieldId(kHealthFraction)));
    CY_CHECK_FALSE(delta->record.contains(reflect::FieldId(kHealthDebugCounter)));
    CY_CHECK_EQ(delta->record.size(), 1U);
}

CY_TEST_CASE("a destroyed authored entity becomes a tombstone and drops its values") {
    // "A destroyed bridge stays destroyed": the authored cell loads unchanged and the tombstone
    // prevents instantiation.
    Overlay overlay = make_overlay();
    record_health(overlay, kVillage, entity(1), 3);
    CY_REQUIRE(overlay.destroy_entity(kVillage, entity(1)).has_value());

    const Entry* entry = overlay.find_entry(kVillage, entity(1));
    CY_REQUIRE(entry != nullptr);
    CY_CHECK_EQ(entry->kind, EntryKind::Tombstone);
    CY_CHECK(entry->components.empty());
    CY_CHECK_EQ(overlay.count_of_kind(EntryKind::Tombstone), 1U);
}

CY_TEST_CASE("a runtime entity created and destroyed leaves nothing behind") {
    // A tombstone for it would make a save that resurrects and re-destroys it on every load.
    Overlay overlay = make_overlay();
    CY_REQUIRE(overlay.create_entity(kVillage, entity(7), AssetId(1, 2)).has_value());
    Structure wall;
    wall.material = 4;
    CY_REQUIRE(
        overlay.record_component(kVillage, entity(7), structure_type(), &wall, 1).has_value());
    CY_REQUIRE(overlay.destroy_entity(kVillage, entity(7)).has_value());

    CY_CHECK(overlay.find_entry(kVillage, entity(7)) == nullptr);
    CY_CHECK_EQ(overlay.count_of_kind(EntryKind::Tombstone), 0U);
}

CY_TEST_CASE("a built structure is a template, an owner and its overrides") {
    // "A built structure returns": recorded as a template, transform, ownership and persistent
    // overrides, not as a dump of its runtime components.
    Overlay overlay = make_overlay();
    CY_REQUIRE(overlay.create_entity(kVillage, entity(7), AssetId(9, 9), entity(1)).has_value());
    Structure wall;
    wall.material = 4;
    wall.integrity = 88;
    CY_REQUIRE(
        overlay.record_component(kVillage, entity(7), structure_type(), &wall, 1).has_value());

    const Entry* entry = overlay.find_entry(kVillage, entity(7));
    CY_REQUIRE(entry != nullptr);
    CY_CHECK_EQ(entry->kind, EntryKind::Created);
    CY_CHECK(entry->template_asset == AssetId(9, 9));
    CY_CHECK(entry->owner == entity(1));
    CY_CHECK_EQ(entry->components.size(), 1U);
}

CY_TEST_CASE("writing to a tombstoned entity is refused rather than resurrecting it") {
    Overlay overlay = make_overlay();
    CY_REQUIRE(overlay.destroy_entity(kVillage, entity(1)).has_value());
    Health health;
    CY_CHECK_FALSE(
        overlay.record_component(kVillage, entity(1), health_type(), &health, 1).has_value());
}

CY_TEST_CASE("a save inspects dirty records rather than the world") {
    // "An autosave in a world of ten million persistent objects SHALL cost work proportional to
    // what changed."
    Overlay overlay = make_overlay();
    for (u64 index = 0; index < 64; ++index) {
        record_health(overlay, RegionKey(0x0B00'0000'0000'0000ULL + index), entity(index), 1);
    }
    CY_CHECK_EQ(overlay.dirty_region_count(), 64U);
    overlay.clear_dirty();
    CY_CHECK_EQ(overlay.dirty_region_count(), 0U);
    CY_CHECK_EQ(overlay.dirty_entry_count(), 0U);

    record_health(overlay, RegionKey(0x0B00'0000'0000'0005ULL), entity(5), 2);
    Array<RegionKey> dirty(test_allocator());
    CY_REQUIRE(overlay.dirty_regions(dirty).has_value());
    CY_REQUIRE_EQ(dirty.size(), 1U);
    CY_CHECK(dirty[0] == RegionKey(0x0B00'0000'0000'0005ULL));
    CY_CHECK_EQ(overlay.dirty_entry_count(), 1U);
    // The other sixty-three regions were not walked to discover that. entry_count() is what a scan
    // would have cost.
    CY_CHECK_EQ(overlay.entry_count(), 64U);
}

CY_TEST_CASE("a change survives its region unloading") {
    // "Dirty state SHALL survive streaming: an entity modified and then unloaded SHALL still
    // contribute its change to the next save."
    Overlay overlay = make_overlay();
    CY_REQUIRE(overlay.set_residency(kVillage, Residency::Resident).has_value());
    record_health(overlay, kVillage, entity(1), 5);
    CY_REQUIRE(overlay.set_residency(kVillage, Residency::Unloaded).has_value());

    CY_CHECK_EQ(overlay.residency_of(kVillage), Residency::Unloaded);
    CY_CHECK_EQ(overlay.resident_region_count(), 0U);
    const ComponentDelta* delta =
        overlay.find_component(kVillage, entity(1), reflect::TypeId(kHealthTypeId));
    CY_REQUIRE(delta != nullptr);
    CY_CHECK_EQ(overlay.dirty_entry_count(), 1U);
}

CY_TEST_CASE("residency does not change what a save contains, byte for byte") {
    // "WHEN the same logical state is saved with different regions resident THEN the saved state
    // SHALL be equivalent." Equivalence is asserted as equality of the encoded bytes, which is
    // stronger and is what the content-addressed chunk store depends on.
    Overlay resident = make_overlay();
    Overlay unloaded = make_overlay();
    for (Overlay* overlay : {&resident, &unloaded}) {
        record_health(*overlay, kVillage, entity(2), 7);
        record_health(*overlay, kVillage, entity(1), 3);
        CY_REQUIRE(overlay->destroy_entity(kQuarry, entity(9)).has_value());
    }
    CY_REQUIRE(resident.set_residency(kVillage, Residency::Resident).has_value());
    CY_REQUIRE(resident.set_residency(kQuarry, Residency::Resident).has_value());

    Array<u8> a(test_allocator());
    Array<u8> b(test_allocator());
    CY_REQUIRE(encode_region(resident, kVillage, a).has_value());
    CY_REQUIRE(encode_region(unloaded, kVillage, b).has_value());
    CY_REQUIRE_EQ(a.size(), b.size());
    CY_CHECK_EQ(std::memcmp(a.data(), b.data(), a.size()), 0);
}

CY_TEST_CASE("insertion order does not change the bytes") {
    // Regions sort by key, entries by identity, components by type, fields by identifier. Two
    // overlays holding the same state are therefore the same save.
    Overlay forwards = make_overlay();
    record_health(forwards, kVillage, entity(1), 3);
    record_health(forwards, kVillage, entity(2), 7);
    record_health(forwards, kVillage, entity(3), 9);

    Overlay backwards = make_overlay();
    record_health(backwards, kVillage, entity(3), 9);
    record_health(backwards, kVillage, entity(1), 3);
    record_health(backwards, kVillage, entity(2), 7);

    Array<u8> a(test_allocator());
    Array<u8> b(test_allocator());
    CY_REQUIRE(encode_region(forwards, kVillage, a).has_value());
    CY_REQUIRE(encode_region(backwards, kVillage, b).has_value());
    CY_REQUIRE_EQ(a.size(), b.size());
    CY_CHECK_EQ(std::memcmp(a.data(), b.data(), a.size()), 0);
}

CY_TEST_CASE("a scope fragment is state with a lifetime, not an entity") {
    Overlay overlay = make_overlay();
    Settings settings;
    settings.gamma = 2.2F;
    settings.language = 3;
    CY_REQUIRE(overlay.record_fragment(Scope::Profile, settings_type(), &settings, 1).has_value());

    CY_CHECK(overlay.find_fragment(Scope::Profile, reflect::TypeId(kSettingsTypeId)) != nullptr);
    CY_CHECK(overlay.find_fragment(Scope::Campaign, reflect::TypeId(kSettingsTypeId)) == nullptr);
    CY_CHECK_EQ(overlay.entry_count(), 0U);
    CY_CHECK_EQ(overlay.dirty_entry_count(), 1U);
}

CY_TEST_CASE("a clone carries the state and the dirty flags") {
    Overlay overlay = make_overlay();
    record_health(overlay, kVillage, entity(1), 3);
    CY_REQUIRE(overlay.set_residency(kQuarry, Residency::Resident).has_value());
    overlay.clear_dirty();
    record_health(overlay, kVillage, entity(2), 4);
    overlay.set_simulation_point(4242);

    Overlay copy(test_allocator());
    CY_REQUIRE(overlay.clone_into(copy).has_value());
    CY_CHECK_EQ(copy.entry_count(), overlay.entry_count());
    CY_CHECK_EQ(copy.dirty_entry_count(), 1U);
    CY_CHECK_EQ(copy.simulation_point(), 4242U);
    // A region that only ever had its residency noted holds no entries, and the clone still has it:
    // copying the flags by index rather than by key would have read the wrong region here.
    CY_CHECK_EQ(copy.residency_of(kQuarry), Residency::Resident);
    CY_CHECK_EQ(copy.region_count(), overlay.region_count());
}

CY_TEST_CASE("merging layers later data over earlier without dropping what it does not mention") {
    Overlay base = make_overlay();
    record_health(base, kVillage, entity(1), 1);
    Structure wall;
    wall.material = 2;
    wall.integrity = 50;
    CY_REQUIRE(base.create_entity(kVillage, entity(7), AssetId(1, 1)).has_value());
    CY_REQUIRE(base.record_component(kVillage, entity(7), structure_type(), &wall, 1).has_value());

    Overlay later = make_overlay();
    record_health(later, kVillage, entity(1), 9);
    CY_REQUIRE(later.destroy_entity(kQuarry, entity(3)).has_value());

    CY_REQUIRE(base.merge(later).has_value());
    const ComponentDelta* health =
        base.find_component(kVillage, entity(1), reflect::TypeId(kHealthTypeId));
    CY_REQUIRE(health != nullptr);
    u32 revives = 0;
    const Span<const u8> bytes = health->record.bytes(reflect::FieldId(kHealthRevives));
    CY_REQUIRE(serialize::decode_scalar(serialize::WireType::U32, bytes.data(),
                                        static_cast<u32>(bytes.size()), &revives)
                   .has_value());
    CY_CHECK_EQ(revives, 9U);
    CY_CHECK(base.find_entry(kVillage, entity(7)) != nullptr);
    CY_CHECK_EQ(base.find_entry(kQuarry, entity(3))->kind, EntryKind::Tombstone);
}
