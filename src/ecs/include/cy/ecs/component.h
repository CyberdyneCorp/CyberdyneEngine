#pragma once
// Component types: what one is, how it is registered, and the mask that identifies a set. Task 2.2.
//
// `ecs-core` — "Components": a component is a reflected struct with no virtual functions, trivially
// relocatable, and storable in packed arrays; and there are five kinds, each with a different
// storage consequence.
//
//   Data     a packed column in the archetype chunk.               Ordinary per-entity data.
//   Tag      zero-sized; presence only, part of the archetype key. Marking and filtering.
//   Shared   an interned value; entities sharing it group into     Render material, LOD group.
//            the same chunks, so the value is per-archetype.
//   Buffer   a variable-length array per entity, inline up to a    Waypoints, inventory, children.
//            capacity and then on the heap (buffer.h).
//   Sparse   a side table keyed by entity; no archetype change.    Rare, frequently toggled data.
//
// REGISTRATION IS NOT AUTOMATIC AND THE REGISTRY IS PER WORLD. `ecs-core` gives a world independent
// archetypes; a component id is an index into *this* world's tables, so two worlds may assign the
// same number to different types and neither is wrong. What is stable across worlds and across
// builds is `reflect::TypeId`, which is what the registry is keyed by and what serialized data
// carries.
//
// THE TWO REGISTRATION ROUTES, AND WHY THERE ARE TWO.
//
//   register_reflected()  the route every game component takes. The descriptor is M1 reflection's,
//                         the key is the manifest identifier, and serialized data addresses the
//                         component by that number.
//   register_builtin()    the engine's own structural components — `Parent` and `Children`
//                         (relationships.h). They are maintained by the world rather than authored,
//                         never appear in a prefab, and carry entity references rather than data,
//                         so they have no manifest identifier to be keyed by. They are keyed by
//                         name instead, and a serialized world names them (snapshot.h) rather than
//                         numbering them.
//
// The second route is a seam, not a category: when src/ecs/'s own headers are wired into the
// reflection generator, `Parent` and `Children` take manifest identifiers and move to the first
// route with no change to anything that consumes them. See README.md.
//
// ENTITY REFERENCE SITES ARE DECLARED, NOT DISCOVERED. A component that holds an `Entity` says
// where, at registration, as byte offsets. Serialization remaps references by walking those
// offsets — a strided pass over known columns — rather than by asking reflection per row what a
// field means. The M2 spike measured the alternative at 4.7-5.2x the cost and named it as the thing
// that would turn cell activation into the reflection walk archetype storage exists to avoid.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/jobs/access.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/core/reflect/registry.h>

namespace cy::ecs {

/// A component type's index within one world. The same numbering the job system's access
/// declarations use, because a system's `Read<T>` and this are the same identifier — see task 2.5.
using ComponentTypeId = jobs::ComponentTypeId;

inline constexpr ComponentTypeId kInvalidComponent = 0xFFFF'FFFFu;

/// The most component types one world may register. Fixed because `ComponentMask` is a value that
/// is copied, compared and hashed on the archetype lookup path, and a growable bitset there would
/// make an archetype's identity an allocation.
inline constexpr u32 kMaxComponentTypes = 256;

/// The most `Entity` fields one component may declare. A component holding more references than
/// this is expressing a relation, which is what `Children` and a buffer component are for.
inline constexpr u32 kMaxEntityRefsPerComponent = 8;

enum class ComponentKind : u8 {
    Data = 0,
    Tag = 1,
    Shared = 2,
    Buffer = 3,
    Sparse = 4,
};

const char* component_kind_name(ComponentKind kind) noexcept;

/// True for the kinds that are part of an archetype's identity, and therefore for the kinds whose
/// addition or removal moves an entity between chunks. Sparse is the one that is not, which is the
/// whole reason it exists.
[[nodiscard]] constexpr bool kind_changes_archetype(ComponentKind kind) noexcept {
    return kind != ComponentKind::Sparse;
}

/// True for the kinds that occupy a column in the chunk. Tag has no data and Shared's value is per
/// archetype, so neither does; Sparse lives in a side table.
[[nodiscard]] constexpr bool kind_has_column(ComponentKind kind) noexcept {
    return kind == ComponentKind::Data || kind == ComponentKind::Buffer;
}

/// The set of component types on an entity — an archetype's identity, and a query's constraint.
class ComponentMask {
public:
    static constexpr u32 kWords = kMaxComponentTypes / 64;

    constexpr ComponentMask() noexcept = default;

    constexpr void set(ComponentTypeId component) noexcept {
        words_[component / 64U] |= (u64{1} << (component % 64U));
    }
    constexpr void clear(ComponentTypeId component) noexcept {
        words_[component / 64U] &= ~(u64{1} << (component % 64U));
    }
    [[nodiscard]] constexpr bool test(ComponentTypeId component) const noexcept {
        return (words_[component / 64U] & (u64{1} << (component % 64U))) != 0;
    }

    /// True when every bit of `other` is set here. The query's `With<T...>` test.
    [[nodiscard]] constexpr bool contains(const ComponentMask& other) const noexcept {
        for (u32 word = 0; word < kWords; ++word) {
            if ((words_[word] & other.words_[word]) != other.words_[word]) {
                return false;
            }
        }
        return true;
    }

    /// True when any bit is set in both. The query's `Without<T...>` test.
    [[nodiscard]] constexpr bool intersects(const ComponentMask& other) const noexcept {
        for (u32 word = 0; word < kWords; ++word) {
            if ((words_[word] & other.words_[word]) != 0) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] constexpr bool empty() const noexcept {
        u64 combined = 0;
        for (const u64 word : words_) {
            combined |= word;
        }
        return combined == 0;
    }

    [[nodiscard]] u32 count() const noexcept;
    [[nodiscard]] u64 hash() const noexcept;

    [[nodiscard]] constexpr u64 word(u32 index) const noexcept { return words_[index]; }

    friend constexpr bool operator==(const ComponentMask& first,
                                     const ComponentMask& second) noexcept {
        for (u32 index = 0; index < kWords; ++index) {
            if (first.words_[index] != second.words_[index]) {
                return false;
            }
        }
        return true;
    }

private:
    u64 words_[kWords] = {};
};

/// Release whatever a component owns outside the chunk. Only a buffer component has one: it frees
/// the heap block a buffer spilled into. Null for every other kind, and the archetype skips the
/// column entirely when it is.
using ComponentReleaseFn = void (*)(void* element, Allocator& allocator) noexcept;

/// What one registered component type is.
struct ComponentInfo {
    ComponentTypeId id = kInvalidComponent;
    ComponentKind kind = ComponentKind::Data;

    /// The reflected descriptor, or null for a built-in. `name` is filled from it when present, so
    /// a diagnostic reads the same either way.
    const reflect::TypeInfo* type = nullptr;
    reflect::TypeId type_id;
    const char* name = "";

    /// Bytes per row in the chunk column. Zero for Tag, Shared and Sparse, which have no column.
    u32 size = 0;
    u32 alignment = 1;

    /// Bytes of the value held outside the chunk: a Shared component's interned value, or a Sparse
    /// component's side-table entry.
    u32 value_size = 0;

    /// Buffer components only: the element and how many fit before the heap is reached.
    u32 element_size = 0;
    u32 element_alignment = 1;
    u32 inline_capacity = 0;
    /// True when a buffer's elements are entity references, so serialization remaps them.
    bool elements_are_entities = false;

    ComponentReleaseFn release = nullptr;

    /// Byte offsets, within one element of this component, of the `Entity` fields it holds.
    /// Declared at registration; see the header comment. Held inline rather than as a pointer into
    /// a side array, so a `ComponentInfo` stays a value that can be copied into a snapshot's
    /// descriptor table without dragging a second allocation behind it.
    u32 entity_offsets[kMaxEntityRefsPerComponent] = {};
    u32 entity_offset_count = 0;
};

/// How a caller describes a component that is not a plain data struct. Every field has a
/// default that means "an ordinary data component", so a data registration passes nothing.
struct ComponentOptions {
    ComponentKind kind = ComponentKind::Data;
    /// Buffer components: the element type's size, alignment, and how many fit inline.
    u32 element_size = 0;
    u32 element_alignment = 1;
    u32 inline_capacity = 0;
    bool elements_are_entities = false;
    /// Buffer components: what frees the heap block a buffer spilled into. `release_buffer<T>`
    /// from buffer.h; null for every other kind, and ignored if given for one.
    ComponentReleaseFn release = nullptr;
    /// Byte offsets of the `Entity` fields within one element. Borrowed and copied.
    Span<const u32> entity_offsets;
};

/// One world's component types.
///
/// Registration order is the id order, and the id order is what an archetype's sorted column list
/// and a snapshot's descriptor table are written in. Two worlds that register in the same order
/// agree on every number, which is what makes a snapshot from one restorable into the other.
class ComponentRegistry {
public:
    explicit ComponentRegistry(Allocator& allocator) noexcept
        : infos_(allocator), by_type_(allocator) {}

    /// Register a reflected type. The route every game component takes.
    ///
    /// Refuses a type that is not trivially relocatable: chunk compaction moves a row's bytes with
    /// `memcpy` and nothing else, so a type whose move is not a copy of its bytes would be silently
    /// corrupted rather than diagnosed. `TypeInfo` records the property, which is why it is
    /// checked here rather than assumed.
    [[nodiscard]] Expected<ComponentTypeId, Error> register_reflected(
        const reflect::TypeInfo& type, const ComponentOptions& options = {}) noexcept;

    /// Register one of the engine's own structural components. See the header comment.
    [[nodiscard]] Expected<ComponentTypeId, Error> register_builtin(
        const char* name, u32 size, u32 alignment, const ComponentOptions& options = {}) noexcept;

    /// The reflected type's component id in this world, or nothing when it was never registered.
    [[nodiscard]] const ComponentInfo* find(reflect::TypeId type) const noexcept;
    [[nodiscard]] const ComponentInfo* find(const char* name) const noexcept;

    [[nodiscard]] const ComponentInfo& info(ComponentTypeId component) const noexcept {
        CY_ASSERT_MSG(component < infos_.size(), "component id out of range");
        return infos_[component];
    }

    [[nodiscard]] bool registered(ComponentTypeId component) const noexcept {
        return component < infos_.size();
    }

    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(infos_.size()); }

private:
    [[nodiscard]] Expected<ComponentTypeId, Error> add(ComponentInfo info,
                                                       const ComponentOptions& options) noexcept;

    Array<ComponentInfo> infos_;
    /// Keyed by `reflect::TypeId::value()`. Built-ins have no type id and are found by name, which
    /// is a scan over a handful of entries and happens once per world at registration.
    HashMap<u32, ComponentTypeId> by_type_;
};

/// The id of a reflected component type in a world's registry, resolved once.
///
/// Deliberately a free function taking the registry rather than a static: a component id is
/// per world (see the header comment), and a static would make it per process.
template <class T>
[[nodiscard]] ComponentTypeId component_id_of(const ComponentRegistry& registry) noexcept {
    const ComponentInfo* info = registry.find(reflect::type_id_of<T>());
    return (info == nullptr) ? kInvalidComponent : info->id;
}

}  // namespace cy::ecs
