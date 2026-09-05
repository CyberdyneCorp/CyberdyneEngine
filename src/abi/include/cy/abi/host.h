// cy/abi/host.h — what is behind `CyEngine` and `CyWorld`. Tasks 2.4, 2.5, 2.6.
//
// The ABI hands out opaque pointers. This header is where they stop being opaque, and the two
// structs below are *defined in the global namespace* rather than in `cy::abi` on purpose: the C
// header declares `struct CyEngine_T*`, so making the opaque tag be the C++ class means the handle
// conversion is a pointer of the right type all the way down, instead of a `reinterpret_cast` that
// no compiler can check and no reader can verify. `cy::abi::Host` and `cy::abi::World` are aliases
// for engine code, so nothing inside the engine has to spell a trailing underscore-T.
//
// --- WHAT THE HOST OWNS -------------------------------------------------------------------------
//
//   * the allocator every ABI allocation comes from, and the live count of heap-backed `CyVar`s
//     that `native-abi`'s "development builds SHALL detect leaks of returned values" asks for;
//   * the behaviour type registry, and the **generation** each registration belongs to;
//   * optionally, a bound world.
//
// It owns no ECS storage, no scene and no renderer: it is the object the ABI's `CyEngine` handle
// addresses, and everything it reaches, it reaches through a pointer the embedder gave it.
//
// --- WHY BEHAVIOUR RECORDS ARE INDIVIDUALLY ALLOCATED --------------------------------------------
//
// `CyBehaviourType` is a pointer a module keeps. If the records lived in a growable array, then
// registering the eleventh behaviour would reallocate and every handle the module already holds
// would dangle — silently, because the memory is still mapped and still looks like a record. So
// each record is its own allocation and the array holds pointers: registration is rarer than a
// frame and the indirection costs nothing that matters.
//
// --- WHY A WORLD CARRIES AN EPOCH ---------------------------------------------------------------
//
// `native-abi` forbids a raw pointer into chunk storage from outliving a structural change, and
// requires that use past that point be detected. The world binding therefore counts structural
// changes, `CyBorrow` carries the count it was taken at, and `borrow_valid` compares. It is a
// comparison rather than an assertion, so it holds in Profile and Shipping too — `CY_ASSERT` is
// compiled out of both, and a rule that only holds in two configurations is not a rule.

#pragma once

#include <cy/abi/cy_abi.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/ecs/component.h>
#include <cy/ecs/entity.h>

#include <atomic>

namespace cy::ecs {
class World;
}  // namespace cy::ecs

namespace cy::abi {

/// One reflected field of a module-registered component, as the engine kept it.
///
/// `name` is borrowed. The C header states the rule at the descriptor — a name must outlive the
/// registration — and under the reload model that is free: a retired module image is never
/// unloaded, so its string literals stay mapped for the process lifetime.
struct FieldRecord {
    const char* name = "";
    CyVarType type = CY_VAR_NIL;
    u32 offset = 0;
    u32 size = 0;
};

/// A component type a module registered, and where its fields are in the world's flat field array.
///
/// The fields are held in one array shared by every component rather than one array per component,
/// because `cy::Array<cy::Array<T>>` would need the inner array to be trivially relocatable and it
/// is not. A (first, count) pair into a flat array is the same information with no such condition.
struct ComponentRecord {
    ecs::ComponentTypeId id = 0;
    const char* name = "";
    u32 size = 0;
    u32 field_first = 0;
    u32 field_count = 0;
};

}  // namespace cy::abi

/// The object `CyBehaviourType` addresses: a behaviour type registration, and the generation of the
/// module image that made it.
///
/// The vtable is a **copy of the prefix both sides agree on**: the engine memcpy's
/// `min(vtable->struct_size, sizeof(CyBehaviourVTable))` bytes over a zeroed struct, so a module
/// compiled against a longer vtable than this engine knows loses only the entries this engine could
/// not have called, and a module compiled against a shorter one leaves the rest null.
struct CyBehaviourType_T {
    const char* name = "";
    CyBehaviourVTable vtable{};
    cy::u32 generation = 0;
};

/// The object `CyWorld` addresses: an ECS world, the module-registered types in it, and the
/// structural epoch borrowed pointers are checked against.
struct CyWorld_T {
    CyWorld_T(cy::Allocator& abi_allocator, cy::ecs::World& ecs_world) noexcept;

    CyWorld_T(const CyWorld_T&) = delete;
    CyWorld_T& operator=(const CyWorld_T&) = delete;
    CyWorld_T(CyWorld_T&&) = delete;
    CyWorld_T& operator=(CyWorld_T&&) = delete;
    ~CyWorld_T() = default;

    /// Register a component type described by a module, or return the id an equal earlier
    /// registration got. Idempotent by name, because a reloaded module registers its types again
    /// and a second registration must not be an error — see cy/abi/module.h.
    [[nodiscard]] cy::Expected<CyComponentTypeId, cy::Error> register_component(
        const CyComponentTypeDesc& desc) noexcept;

    [[nodiscard]] const cy::abi::ComponentRecord* find(const char* name) const noexcept;
    [[nodiscard]] const cy::abi::ComponentRecord* record(CyComponentTypeId id) const noexcept;
    [[nodiscard]] const cy::abi::FieldRecord* field(const cy::abi::ComponentRecord& component,
                                                    cy::u32 index) const noexcept;

    /// Note that a structural change happened. Every entry that adds, removes, creates or destroys
    /// calls it, which is what makes a stale `CyBorrow` detectable rather than merely unlucky.
    void bump_epoch() noexcept { ++epoch; }

    cy::ecs::World& world;
    cy::u64 epoch = 1;  ///< Never zero, so a default-constructed CyBorrow is never valid.
    cy::Array<cy::abi::ComponentRecord> components;
    cy::Array<cy::abi::FieldRecord> fields;
};

/// The object `CyEngine` addresses.
struct CyEngine_T {
    explicit CyEngine_T(cy::Allocator& abi_allocator) noexcept;

    CyEngine_T(const CyEngine_T&) = delete;
    CyEngine_T& operator=(const CyEngine_T&) = delete;
    CyEngine_T(CyEngine_T&&) = delete;
    CyEngine_T& operator=(CyEngine_T&&) = delete;
    ~CyEngine_T();

    /// Bind the world modules see through `engine_world`. The binding is owned by the caller and
    /// must outlive the host; the host holds a pointer and never a copy, because there is exactly
    /// one world and two views of it would be two worlds.
    void bind_world(CyWorld_T* binding) noexcept { world = binding; }

    /// Register a behaviour type in the current generation. Re-registering a name that belongs to
    /// the current generation replaces the vtable; re-registering one from a *retired* generation
    /// appends a new record, so the old generation's instances keep resolving to the code that
    /// created them.
    [[nodiscard]] cy::Expected<CyBehaviourType_T*, cy::Error> register_behaviour(
        const char* name, const CyBehaviourVTable& vtable) noexcept;

    /// The record of that name in the current generation, or null. A retired generation's record is
    /// deliberately not findable by name: an instance reaches its own vtable through the handle it
    /// was created with, never through a lookup.
    [[nodiscard]] CyBehaviourType_T* find_behaviour(const char* name) noexcept;

    /// Retire the current generation and open the next. Called by the loader before the next
    /// image's entry point runs; see cy/abi/module.h for the whole sequence.
    void open_generation() noexcept { ++generation; }

    /// Undo `open_generation`, discarding every registration the generation being abandoned made.
    ///
    /// This is what makes a refused reload leave nothing behind. The new image's entry point may
    /// have registered several types before it decided it could not continue, and those records
    /// would otherwise sit in the array forever — invisible, because lookup filters on the current
    /// generation, but never freed. Retiring a generation that has live instances is a programmer
    /// error and the loader never does it: it abandons only a generation it has not yet moved any
    /// instance into.
    void abandon_generation() noexcept;

    cy::Allocator& allocator;
    CyWorld_T* world = nullptr;
    cy::Array<CyBehaviourType_T*> behaviours;
    cy::u32 generation = 0;
    /// Heap-backed `CyVar` payloads currently alive. Atomic because a value may be released on a
    /// job worker, and counted in every configuration so that a leak assertion is evidence rather
    /// than a development-only courtesy.
    std::atomic<cy::u64> live_vars{0};
};

namespace cy::abi {

using Host = ::CyEngine_T;
using World = ::CyWorld_T;
using BehaviourRecord = ::CyBehaviourType_T;

/// The flat entity id the ABI carries, and back. `cy::ecs::Entity` packs index low and generation
/// high, which is exactly what `bits()` and `from_bits()` do — stated here so that no other file in
/// the ABI open-codes the shift.
[[nodiscard]] inline CyEntity to_abi(ecs::Entity entity) noexcept {
    return static_cast<CyEntity>(entity.bits());
}
[[nodiscard]] inline ecs::Entity from_abi(CyEntity entity) noexcept {
    return ecs::Entity::from_bits(static_cast<u64>(entity));
}

/// The byte width of a `CyVarType` when it is stored inside a component. Zero for the types that
/// have no fixed width in storage — nil, string and bytes — which is what makes a field of one of
/// those a registration error rather than a silent misread.
[[nodiscard]] u32 var_type_storage_size(CyVarType type) noexcept;

}  // namespace cy::abi
