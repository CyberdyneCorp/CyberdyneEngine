// The reflection module, in one include. Sections 1.1 and 1.2, governed by `core-type-system`.
//
// Include this from control-plane code — an inspector, a serializer, a migration, a binding
// generator. Do not include it from a header that is itself reflected: annotations.h is all such a
// header needs, and it includes nothing, which is what keeps the generator's parse cheap.

#ifndef CY_CORE_REFLECT_REFLECT_H
#define CY_CORE_REFLECT_REFLECT_H

#include <cy/core/reflect/annotations.h>
#include <cy/core/reflect/attributes.h>
#include <cy/core/reflect/control_plane.h>
#include <cy/core/reflect/ids.h>
#include <cy/core/reflect/registry.h>
#include <cy/core/reflect/serialize.h>
#include <cy/core/reflect/type_info.h>

namespace cy::reflect {

/// Register every type the generator emitted for this link, in a deterministic order.
///
/// Defined by the generated aggregate translation unit, not by this module: the set of reflected
/// types is a property of what was built, and a hand-written list of them would be the parallel
/// metadata definition the generator exists to abolish.
///
/// Idempotent — registering the same descriptors twice succeeds and changes nothing.
Status register_generated_types(TypeRegistry& registry);

/// The same, into default_registry(). What a host calls at startup.
Status register_generated_types();

}  // namespace cy::reflect

#endif  // CY_CORE_REFLECT_REFLECT_H
