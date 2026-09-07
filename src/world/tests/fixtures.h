#pragma once
// The component set and the cell-cooking helper CyberWorld's suites are written against.
//
// The descriptors are hand-written for the reason src/ecs/tests/fixtures.h gives: a
// `reflect::TypeInfo` is plain constexpr data and the generator emits exactly this shape, and the
// generator's annotated-header list is not this module's to edit. The identifiers start at 9100 so
// that a number here is obviously not one identity/manifest.toml issued; they are never registered
// into `reflect::default_registry()` and live only inside a test's own per-world registry.

#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/type_info.h>
#include <cy/ecs/world.h>
#include <cy/world/cell.h>
#include <cy/world/partition.h>

namespace cy::world::test {

/// Where a prop is, in the cell's local frame. `world-partition-and-streaming`'s cell-relative form
/// is what the world persists; a component holds the simulation-local value the systems read.
struct Placement {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
};

struct Prop {
    u32 kind = 0;
    u32 variant = 0;
};

template <class T>
[[nodiscard]] inline const reflect::TypeInfo& descriptor(const char* name, u32 id) noexcept {
    static reflect::TypeInfo info;
    info.name = name;
    info.id = reflect::TypeId(id);
    info.size = static_cast<u32>(sizeof(T));
    info.alignment = static_cast<u32>(alignof(T));
    info.trivially_relocatable = true;
    return info;
}

[[nodiscard]] inline const reflect::TypeInfo& placement_type() noexcept {
    return descriptor<Placement>("cy::world::test::Placement", 9101);
}

[[nodiscard]] inline const reflect::TypeInfo& prop_type() noexcept {
    return descriptor<Prop>("cy::world::test::Prop", 9102);
}

struct Components {
    ecs::ComponentTypeId placement = ecs::kInvalidComponent;
    ecs::ComponentTypeId prop = ecs::kInvalidComponent;
};

[[nodiscard]] inline Expected<Components, Error> register_components(ecs::World& world) noexcept {
    Components ids;
    Expected<ecs::ComponentTypeId, Error> placement =
        world.components().register_reflected(placement_type());
    if (!placement) {
        return make_unexpected(placement.error());
    }
    ids.placement = *placement;
    Expected<ecs::ComponentTypeId, Error> prop = world.components().register_reflected(prop_type());
    if (!prop) {
        return make_unexpected(prop.error());
    }
    ids.prop = *prop;
    return ids;
}

/// Compare two C strings by content. `CY_CHECK_EQ` on two `const char*` compares POINTERS, which
/// happens to pass when the linker merges two identical literals and fails when it does not.
[[nodiscard]] inline bool same_text(const char* left, const char* right) noexcept {
    if (left == nullptr || right == nullptr) {
        return left == right;
    }
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

[[nodiscard]] inline Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

/// A cell of `rows` props, all in one layer, with predictable values: the row index in
/// `Placement::x` and `Prop::kind`, so a test can tell which row landed where after a bulk copy.
///
/// `first_id` is the persistent identifier of row 0; the rest follow. Identifiers are the caller's
/// because "duplicate persistent identifiers SHALL be a cook error" is one of the things under
/// test.
[[nodiscard]] inline CookedCell cook_props(Allocator& memory, const Partitioner& partitioner,
                                           CellCoord coord, u32 rows, const Components& ids,
                                           u64 first_id, LayerId layer = kDefaultLayer) noexcept {
    CellBuilder builder(memory, partitioner.id_of(coord), coord);
    const ecs::ComponentTypeId components[] = {ids.placement, ids.prop};
    for (u32 row = 0; row < rows; ++row) {
        Placement placement;
        placement.x = static_cast<f32>(row);
        Prop prop;
        prop.kind = row;
        prop.variant = 1;
        const void* values[] = {&placement, &prop};
        const u32 sizes[] = {static_cast<u32>(sizeof(Placement)), static_cast<u32>(sizeof(Prop))};
        // The suites assert on the finished cell; a failure here would show up as a row count that
        // does not match, which is the same signal and does not need the builder to be fallible in
        // the test's own control flow.
        (void)builder.add_entity(PersistentId{first_id + row}, layer, components, values, sizes);
    }
    CellPayload payload;
    payload.channel = Channel::Geometry;
    payload.size_bytes = 4096;
    (void)builder.add_payload(payload);
    return builder.finish();
}

}  // namespace cy::world::test
