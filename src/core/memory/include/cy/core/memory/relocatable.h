#pragma once
// Trivial relocatability, and what the containers do with it. Task 2.4.
//
// `core-memory-and-containers` — "Trivially relocatable types move cheaply": when an `Array<T>`
// grows and `T` is trivially relocatable, elements are moved with a single memcpy rather than
// per-element move construction.
//
// A type is trivially relocatable when moving its bytes to a new address and not destroying the
// original is equivalent to move-constructing at the new address and destroying the old one. Every
// trivially copyable type qualifies, and so do many that are not — a `cy::Array` is one: it owns a
// pointer, and the pointer does not care where the owner lives. A type that is NOT trivially
// relocatable is one whose object graph points back at itself: an intrusive list node whose
// neighbours point at it, or anything holding a pointer into its own storage.
//
// C++ has no way to detect this, so it is declared. The default is `std::is_trivially_copyable`,
// which is always correct, and `CY_DECLARE_TRIVIALLY_RELOCATABLE` widens it for a type whose author
// has checked. Declaring it wrongly is a memory-corruption bug with no diagnostic, which is why the
// macro names the type rather than a whole category.

#include <type_traits>

namespace cy {

/// Specialise to widen the default. The primary template is the safe answer.
template <class T>
struct IsTriviallyRelocatable : std::bool_constant<std::is_trivially_copyable_v<T>> {};

template <class T>
inline constexpr bool is_trivially_relocatable_v = IsTriviallyRelocatable<T>::value;

}  // namespace cy

/// Declare `type` trivially relocatable. At namespace scope, outside `cy`.
///
///   CY_DECLARE_TRIVIALLY_RELOCATABLE(cy::Array<int>);
#define CY_DECLARE_TRIVIALLY_RELOCATABLE(type)               \
    namespace cy {                                           \
    template <>                                              \
    struct IsTriviallyRelocatable<type> : std::true_type {}; \
    }
