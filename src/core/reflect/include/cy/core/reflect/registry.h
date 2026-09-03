// The opt-in type registry. Task 1.1.1.
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
// same registry in the same order.
//
// The specification offers `cy::reflect<T>()` as an alternative spelling of the opt-in. The
// annotation is the route this engine takes, and `reflect` is the namespace, so the type-to-
// metadata direction is spelled type_of<T>() below — a function template and a namespace cannot
// share a name, and the namespace is the one every consumer types.
//
// Lookup is a linear scan. That is a deliberate non-optimisation: this is control-plane code, the
// registry holds hundreds of entries rather than millions, and anything that needed it to be faster
// would be doing per-frame work through reflection, which is the thing task 1.1.4 exists to catch.

#ifndef CY_CORE_REFLECT_REGISTRY_H
#define CY_CORE_REFLECT_REGISTRY_H

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/reflect/ids.h>
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
    /// translation unit and outlives the registry.
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

    [[nodiscard]] usize size() const noexcept { return count_; }
    [[nodiscard]] const TypeInfo* const* begin() const noexcept { return entries_; }
    [[nodiscard]] const TypeInfo* const* end() const noexcept { return entries_ + count_; }

    /// Forget every registration. For tests, and for a host that tears down and rebuilds.
    void clear() noexcept;

private:
    Status reserve(usize wanted);

    const TypeInfo** entries_ = nullptr;
    usize count_ = 0;
    usize capacity_ = 0;
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
