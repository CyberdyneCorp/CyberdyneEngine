#pragma once
// The declarations CyberField's suites are written against.
//
// They are deliberately NOT the standard fields' real declarations: the meaning, unit, resolution
// and default of `moisture` belong to whichever M10 row produces it, and a fixture that fixed them
// here would be this module having terrain's opinion. What these declare is one field of each SHAPE
// the substrate has to carry — a quantised scalar, an f32 vector, an integer category, a volumetric
// field, a gameplay-visible one and a visual one — because the shapes are what the substrate is
// responsible for.

#include <cy/core/memory/system_allocator.h>
#include <cy/environment/field.h>
#include <cy/environment/store.h>
#include <cy/world/coordinates.h>

namespace cy::environment::test {

[[nodiscard]] inline Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

/// A partition with 128 m level-0 cells at the origin. The same shape src/world/'s own fixtures
/// use, so a cell footprint computed here is the one that module would compute.
[[nodiscard]] inline world::PartitionConfig partition() noexcept {
    world::PartitionConfig config;
    config.partition = 1;
    config.base_cell_size = 128.0F;
    config.levels = 3;
    config.level_ratio = 4;
    return config;
}

/// A quantised scalar in [0, 1] at three resolutions, macro resident everywhere: the shape a
/// moisture or a wetness field has. Gameplay-visible, so it also exercises every rule §1.4 adds.
[[nodiscard]] inline FieldDeclaration moisture_like() noexcept {
    FieldDeclaration declaration;
    declaration.name = "test.moisture";
    declaration.unit = "fraction";
    declaration.semantics = "water held in the top soil layer, 0 bone dry to 1 saturated";
    declaration.type = FieldType::Scalar;
    declaration.encoding = FieldEncoding::UNorm8;
    declaration.interpolation = FieldInterpolation::Linear;
    declaration.cadence = FieldCadence::SlowlyVarying;
    declaration.production = FieldProduction::Cpu;
    declaration.range_min = 0.0F;
    declaration.range_max = 1.0F;
    declaration.default_value = FieldValue::scalar(0.25F);
    declaration.levels[0] = FieldLevel{2.0F, false};
    declaration.levels[1] = FieldLevel{8.0F, false};
    declaration.levels[2] = FieldLevel{64.0F, true};
    declaration.classification = determinism::SimulationClass::Persistent;
    declaration.gameplay_level = FieldResidency::Macro;
    declaration.persistent = true;
    return declaration;
}

/// A three-component f32 field over a column of cells: the shape wind has. Presentation-classified,
/// so `sample()` takes the finest-resident path.
[[nodiscard]] inline FieldDeclaration wind_like() noexcept {
    FieldDeclaration declaration;
    declaration.name = "test.wind";
    declaration.unit = "m/s";
    declaration.semantics = "air velocity, world axes";
    declaration.type = FieldType::Vec3;
    declaration.encoding = FieldEncoding::F32;
    declaration.interpolation = FieldInterpolation::Linear;
    declaration.cadence = FieldCadence::PerFrame;
    declaration.range_min = -40.0F;
    declaration.range_max = 40.0F;
    declaration.default_value = FieldValue::vec3(1.0F, 0.0F, 0.0F);
    declaration.levels[0] = FieldLevel{4.0F, false};
    declaration.levels[2] = FieldLevel{32.0F, true};
    declaration.vertical_cells = 4;
    declaration.vertical_metres = 25.0F;
    declaration.vertical_origin_metres = 0.0F;
    declaration.classification = determinism::SimulationClass::Presentation;
    return declaration;
}

/// An integer category: the shape biome and soil have. Nearest by necessity — an averaged category
/// names nothing — and the substrate refuses the other pairing.
[[nodiscard]] inline FieldDeclaration biome_like() noexcept {
    FieldDeclaration declaration;
    declaration.name = "test.biome";
    declaration.unit = "index";
    declaration.semantics = "the project's biome table index";
    declaration.type = FieldType::Category;
    declaration.encoding = FieldEncoding::Uint8;
    declaration.interpolation = FieldInterpolation::Nearest;
    declaration.cadence = FieldCadence::Static;
    declaration.range_min = 0.0F;
    declaration.range_max = 255.0F;
    declaration.default_value = FieldValue::category(0);
    declaration.levels[0] = FieldLevel{8.0F, false};
    declaration.levels[2] = FieldLevel{64.0F, true};
    declaration.classification = determinism::SimulationClass::Persistent;
    declaration.gameplay_level = FieldResidency::Macro;
    return declaration;
}

/// A project field the engine has never heard of. The specification's "Project field" scenario:
/// "WHEN a project declares a `radiation` field, THEN it SHALL stream, sample, and debug like a
/// standard field with no engine change."
[[nodiscard]] inline FieldDeclaration project_radiation() noexcept {
    FieldDeclaration declaration;
    declaration.name = "project.radiation";
    declaration.unit = "sieverts/hour";
    declaration.semantics = "absorbed dose rate at ground level";
    declaration.type = FieldType::Scalar;
    declaration.encoding = FieldEncoding::UNorm16;
    declaration.interpolation = FieldInterpolation::Linear;
    declaration.range_min = 0.0F;
    declaration.range_max = 10.0F;
    declaration.default_value = FieldValue::scalar(0.0F);
    declaration.levels[0] = FieldLevel{4.0F, false};
    declaration.levels[2] = FieldLevel{64.0F, true};
    declaration.classification = determinism::SimulationClass::Persistent;
    declaration.gameplay_level = FieldResidency::Macro;
    return declaration;
}

/// Make one tile resident, filled with `value`, through the producer's own path.
[[nodiscard]] inline Status fill_tile(FieldStore& store, const ProducerToken& token,
                                      const TileAddress& address,
                                      const FieldValue& value) noexcept {
    Expected<FieldWriter, Error> writer = store.open_writer(token);
    if (!writer) {
        return make_unexpected(writer.error());
    }
    if (Status staged = writer->stage(address); !staged) {
        return staged;
    }
    if (Status filled = writer->fill(address, value); !filled) {
        return filled;
    }
    return writer->publish();
}

[[nodiscard]] inline TileAddress tile_at(FieldId field, u8 level, i32 x, i32 z,
                                         FieldLayer layer = FieldLayer::Base) noexcept {
    TileAddress address;
    address.field = field;
    address.level = level;
    address.layer = static_cast<u8>(layer);
    address.x = x;
    address.z = z;
    return address;
}

/// Literal comparison, because a declaration's unit and semantics are literals the producer owns
/// and a test that compared pointers would pass for the wrong reason.
[[nodiscard]] inline bool same_text(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) {
        return a == b;
    }
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

[[nodiscard]] inline world::WorldVec3d at(f64 x, f64 z, f64 y = 0.0) noexcept {
    return world::WorldVec3d{x, y, z};
}

}  // namespace cy::environment::test
