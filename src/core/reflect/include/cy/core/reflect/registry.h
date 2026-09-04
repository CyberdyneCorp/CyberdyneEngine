// The opt-in type registry. Task 1.1.1, revised for M2 task 1.1.
//
// `core-type-system` requires a registry mapping a stable TypeId to type metadata, and requires
// that types opt in **by declaration rather than by inheritance**. There is no common Object base
// here and there never will be: a reflected component is a plain struct, and the registry holds
// pointers to constexpr descriptors that generated code owns.
//
// Nothing is reflected by accident. Three separate acts are needed, and each is visible in a diff:
//
//   1. the type carries CY_REFLECT_TYPE() and its fields carry CY_REFLECT_FIELD(),
//   2. its header is named in a module's CY_REFLECT_HEADERS list in CMake,
//   3. something calls register_generated_types().
//
// Registration is an explicit call rather than a static initialiser on purpose. A static
// initialiser would make the contents of the registry depend on link order, which is exactly the
// class of non-determinism `engine-architecture` spends its startup-ordering requirement removing.
// The generated aggregate registers in sorted order, so two builds of the same tree produce the
// same registry in the same order — and `begin()`/`end()` still iterate in that order, because the
// hash index added below is a second view onto the same entries rather than their storage.
//
// The specification offers `cy::reflect<T>()` as an alternative spelling of the opt-in. The
// annotation is the route this engine takes, and `reflect` is the namespace, so the type-to-
// metadata direction is spelled type_of<T>() below — a function template and a namespace cannot
// share a name, and the namespace is the one every consumer types.
//
// --- LOOKUP COMPLEXITY IS PART OF THE CONTRACT
// ------------------------------------------------------
//
// M1 shipped a linear scan here and said so, on the argument that the registry holds hundreds of
// entries and this is control-plane code. That argument was wrong in one specific way, and the M2
// spec delta closes it: "Type and field lookup SHALL NOT be linear in the number of registered
// types or fields." A scene load calls find() once per record, so a linear scan makes loading cost
// the product of the record count and the registry size — the registry does not have to be on a
// per-frame path for its size to multiply into somebody else's loop.
//
// So both lookups go through an open-addressed index (probe_table.h) whose probe length does not
// grow with the entry count, and the registry additionally builds a FieldIndex per type at
// registration, so that the caller who has a TypeId and a stream of records — the scene loader —
// never scans anything and never builds an index of its own.

#ifndef CY_CORE_REFLECT_REGISTRY_H
#define CY_CORE_REFLECT_REGISTRY_H

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/reflect/field_index.h>
#include <cy/core/reflect/ids.h>
#include <cy/core/reflect/probe_table.h>
#include <cy/core/reflect/type_info.h>

namespace cy::reflect {

class TypeRegistry {
public:
    TypeRegistry() noexcept = default;
    ~TypeRegistry();

    // Copying would leave two owners of one entry table. Moving is fine and is what lets a helper
    // build a registry and hand it back.
    TypeRegistry(const TypeRegistry&) = delete;
    TypeRegistry& operator=(const TypeRegistry&) = delete;
    TypeRegistry(TypeRegistry&& other) noexcept;
    TypeRegistry& operator=(TypeRegistry&& other) noexcept;

    /// Register a type. The descriptor is borrowed, not copied: it is constexpr data in a generated
    /// translation unit and outlives the registry. A FieldIndex for it is built here, so that no
    /// consumer has to build one and no consumer is tempted to scan instead.
    ///
    /// Fails on an invalid TypeId, and on a second registration of the same TypeId by a different
    /// descriptor — which is the shape a recycled identifier takes at run time, and the last place
    /// it can still be caught. Registering the identical descriptor twice succeeds and does
    /// nothing, so a module registered by two consumers is not an error.
    Status add(const TypeInfo& info);

    /// The type with this identifier, or null. A reflected lookup: control plane only.
    [[nodiscard]] const TypeInfo* find(TypeId id) const noexcept;

    /// The type with this fully qualified name, or null. Names are metadata — use this for tooling
    /// and diagnostics, never to persist a reference.
    [[nodiscard]] const TypeInfo* find(const char* name) const noexcept;

    /// The field index built for this type at registration, or null when it is not registered.
    ///
    /// This is what a serializer or a scene loader holds for the duration of a load: it resolves a
    /// FieldId without a scan, and it is already built, so decoding a thousand records of one type
    /// costs one lookup here and a hash probe per field.
    [[nodiscard]] const FieldIndex* fields(TypeId id) const noexcept;

    [[nodiscard]] usize size() const noexcept { return count_; }
    [[nodiscard]] const TypeInfo* const* begin() const noexcept { return entries_; }
    [[nodiscard]] const TypeInfo* const* end() const noexcept { return entries_ + count_; }

    /// The longest probe chain either index holds, measured as it was filled. A diagnostic: it is
    /// what lets a test assert that lookup cost does not grow with the registry without timing
    /// anything. See detail::ProbeTable::longest_probe().
    [[nodiscard]] u32 longest_probe() const noexcept;

    /// Forget every registration. For tests, and for a host that tears down and rebuilds.
    void clear() noexcept;

private:
    Status grow(usize wanted);
    Status rebuild_index(usize slot_count);

    const TypeInfo** entries_ = nullptr;
    FieldIndex* field_indices_ = nullptr;
    usize count_ = 0;
    usize capacity_ = 0;

    /// One block, two tables: `slot_count_` slots keyed by TypeId, then as many keyed by name.
    u32* slots_ = nullptr;
    u32 slot_count_ = 0;
    detail::ProbeTable by_id_;
    detail::ProbeTable by_name_;
};

/// The registry generated registration targets by default, and the one tooling reads.
[[nodiscard]] TypeRegistry& default_registry();

/// The descriptor for a reflected type, known at compile time.
///
/// Generated code specialises this; an unreflected T fails to compile with a message naming the
/// type, which is the opt-in being enforced by the language rather than discovered at run time.
template <typename T>
struct Reflected;

/// The metadata for T. Compiles only for a reflected T.
template <typename T>
[[nodiscard]] const TypeInfo& type_of() noexcept {
    return Reflected<T>::info();
}

/// The identifier for T. Compiles only for a reflected T.
template <typename T>
[[nodiscard]] constexpr TypeId type_id_of() noexcept {
    return Reflected<T>::id;
}

}  // namespace cy::reflect

#endif  // CY_CORE_REFLECT_REGISTRY_H
