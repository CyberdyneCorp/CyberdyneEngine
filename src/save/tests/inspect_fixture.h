#pragma once
// SPDX-License-Identifier: MIT
// The campaign the inspector suites and the committed fixture save are written from.
//
// THREE GENERATIONS OF ONE CAMPAIGN, each committed from an overlay built from nothing — which is
// what `SaveArchive::commit` writes: the whole overlay, deduplicated by chunk hash — so that every
// difference the semantic diff names is one this file put there on purpose:
//
//   generation  tick  village                         quarry            summit       profile
//   ----------  ----  ------------------------------  ----------------  -----------  -------------
//   1           100   e1 revives=1, e2 revives=2      e10 material=3    -            gamma 1.0
//                                                         integrity=90                 language 0
//   2           200   e2 revives=5; e30 spawned from  unchanged         e20          language 2
//                     template (7,7), owned by e1,                      destroyed
//                     material=4 integrity=100
//   3           300   e2 back to authored; e30        integrity=40      unchanged    gamma 1.5
//                     destroyed; e1 gains a plugin's
//                     record (type 9499) this schema
//                     does not declare
//
// src/save/tests/data/inspect-campaign/ is this function's output written to a directory and
// committed. `the committed inspector fixture is what its generator writes` (test_archive.cpp)
// rewrites it into memory and requires every object to be byte-for-byte the committed one, so the
// fixture cannot drift from the code that describes it — and a change to the container's encoding
// shows up there as a failing case rather than as a fixture that quietly stopped meaning what this
// table says. To regenerate after a deliberate format change:
// `CY_SAVE_WRITE_FIXTURE=$PWD/src/save/tests/data/inspect-campaign
// build/<p>/cy_test_integration_save_archive -tc='the committed inspector fixture*'`, then review
// the diff and run the case again without the variable.

#include <cy/core/reflect/registry.h>
#include <cy/save/archive.h>
#include <cy/save/overlay.h>

#include "fixtures.h"

namespace cy::save::test {

inline constexpr RegionKey kFixtureVillage{0x0A00'0000'0000'0001ULL};
inline constexpr RegionKey kFixtureQuarry{0x0A00'0000'0000'0002ULL};
inline constexpr RegionKey kFixtureSummit{0x0A00'0000'0000'0003ULL};

/// A type no schema in this suite declares: a plugin's state the build loading it does not have.
inline constexpr u32 kPluginTypeId = 9499;
inline constexpr u32 kPluginWetness = 1;

inline constexpr u64 kFixtureTicks[] = {100, 200, 300};

[[nodiscard]] inline SaveIdentity fixture_identity() noexcept {
    SaveIdentity id;
    id.build_id = "11.5.0+fixture";
    id.project = AssetId(0xC1, 1);
    id.save = AssetId(0xC1, 2);
    id.campaign = AssetId(0xC1, 3);
    id.session_seed = 0x5EED'F1C5ULL;
    return id;
}

/// The schema the fixture is explained against: the suite's descriptors, each owned by a module so
/// that "size by plugin" has something to attribute to.
[[nodiscard]] inline Status fixture_types(reflect::TypeRegistry& registry) noexcept {
    static reflect::TypeInfo health = health_type();
    static reflect::TypeInfo structure = structure_type();
    static reflect::TypeInfo settings = settings_type();
    health.module = "cy_gameplay";
    structure.module = "cy_building";
    settings.module = "cy_settings";
    for (const reflect::TypeInfo* info : {&health, &structure, &settings}) {
        if (Status added = registry.add(*info); !added) {
            return added;
        }
    }
    return ok();
}

namespace detail {

inline Status fixture_health(Overlay& overlay, PersistentId id, u32 revives) noexcept {
    Health health;
    health.revives = revives;
    return overlay.record_component(kFixtureVillage, id, health_type(), &health, 1);
}

inline Status fixture_structure(Overlay& overlay, RegionKey region, PersistentId id, u32 material,
                                u32 integrity) noexcept {
    Structure structure;
    structure.material = material;
    structure.integrity = integrity;
    return overlay.record_component(region, id, structure_type(), &structure, 1);
}

inline Status fixture_settings(Overlay& overlay, f32 gamma, u32 language) noexcept {
    Settings settings;
    settings.gamma = gamma;
    settings.language = language;
    return overlay.record_fragment(Scope::Profile, settings_type(), &settings, 1);
}

inline Status fixture_plugin_state(Overlay& overlay, PersistentId id) noexcept {
    serialize::ValueRecord record(overlay.allocator());
    record.set_type(reflect::TypeId(kPluginTypeId));
    record.set_schema_version(4);
    const u32 wetness = 70;
    if (Status set = record.set_scalar(reflect::FieldId(kPluginWetness), serialize::WireType::U32,
                                       &wetness, sizeof(wetness));
        !set) {
        return set;
    }
    return overlay.record_component(kFixtureVillage, id, reflect::TypeId(kPluginTypeId), 4, record);
}

/// Generation `index` (0-based) of the table above, as an overlay built from nothing.
inline Status fixture_generation(Overlay& overlay, u32 index) noexcept {
    Status status = fixture_health(overlay, entity(1), 1);
    if (status && index < 2) {
        status = fixture_health(overlay, entity(2), index == 0 ? 2U : 5U);
    }
    if (status) {
        status = fixture_structure(overlay, kFixtureQuarry, entity(10), 3, index == 2 ? 40U : 90U);
    }
    if (status && index >= 1) {
        status = overlay.destroy_entity(kFixtureSummit, entity(20));
    }
    if (status && index == 1) {
        status = overlay.create_entity(kFixtureVillage, entity(30), AssetId(7, 7), entity(1));
        status = status ? fixture_structure(overlay, kFixtureVillage, entity(30), 4, 100) : status;
    }
    if (status && index == 2) {
        status = fixture_plugin_state(overlay, entity(1));
    }
    if (status) {
        status = fixture_settings(overlay, index == 2 ? 1.5F : 1.0F, index == 0 ? 0U : 2U);
    }
    overlay.set_simulation_point(kFixtureTicks[index]);
    return status;
}

}  // namespace detail

/// Commit the three generations into `backend`.
[[nodiscard]] inline Status write_fixture_campaign(SaveBackend& backend,
                                                   Allocator& allocator) noexcept {
    SaveArchive archive(allocator);
    if (Status opened = archive.open(backend); !opened) {
        return opened;
    }
    for (u32 index = 0; index < 3; ++index) {
        Overlay overlay(allocator);
        if (Status built = detail::fixture_generation(overlay, index); !built) {
            return built;
        }
        if (Expected<u32, Error> committed = archive.commit(overlay, fixture_identity());
            !committed) {
            return make_unexpected(committed.error());
        }
    }
    return ok();
}

}  // namespace cy::save::test
