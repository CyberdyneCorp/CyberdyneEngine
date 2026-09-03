// The reflection annotation vocabulary. Task 1.1.1.
//
// A type opts in by carrying CY_REFLECT_TYPE(); a field opts in by carrying CY_REFLECT_FIELD().
// Nothing is reflected by accident: an annotated type is still invisible until its header is named
// in a module's CY_REFLECT_HEADERS list, and the metadata it produces is still inert until
// register_generated_types() is called. Three deliberate acts, not one.
//
// Both macros expand to nothing in a normal compile. The annotation exists only while
// tools/gen/reflect_gen.py is parsing, which defines CY_REFLECT_GENERATOR — so a reflected struct
// is standard-layout, has no base class, no virtual functions and no hidden fields, and compiles
// identically under GCC, which has no [[clang::annotate]].
//
// This header includes nothing, and it must stay that way. The generator parses every annotated
// header with a real C++ frontend, so a reflected header's cost is what it transitively includes:
// the M1 spike measured a ninefold increase in cold generation from two standard-library includes
// per header, at an identical type count. A component struct should not need <string>.
//
// The argument list is stringised and parsed by the generator, so the spelling below is the
// contract:
//
//   struct CY_REFLECT_TYPE() Health {
//       CY_REFLECT_FIELD(Range(0, 1000), Unit(Metres), Category("Combat")) f32 maximum;
//       CY_REFLECT_FIELD(Transient) f32 cached;
//   };
//
// The attribute set is `core-type-system`'s table, and it is closed unless a module declares its
// own: Range, Enum, Flags, Hidden, ReadOnly, Category, Tooltip, Transient, Replicated, AssetRef,
// Unit and Persistence. An unknown or malformed attribute is a generation error naming the field,
// never a value silently dropped. A module adds its own with an attribute schema — see
// tools/gen/attributes/README.md.

#ifndef CY_CORE_REFLECT_ANNOTATIONS_H
#define CY_CORE_REFLECT_ANNOTATIONS_H

#if defined(CY_REFLECT_GENERATOR)

// clang's annotate attribute is the only channel libclang exposes for arbitrary text on a
// declaration. The prefix distinguishes the engine's annotations from anyone else's, and the
// stringised argument list is what the generator parses.
#    define CY_REFLECT_TYPE(...) __attribute__((annotate("cy.type:" #__VA_ARGS__)))
#    define CY_REFLECT_FIELD(...) __attribute__((annotate("cy.field:" #__VA_ARGS__)))
#    define CY_REFLECT_ENUM(...) __attribute__((annotate("cy.enum:" #__VA_ARGS__)))

#else

#    define CY_REFLECT_TYPE(...)
#    define CY_REFLECT_FIELD(...)
#    define CY_REFLECT_ENUM(...)

#endif  // CY_REFLECT_GENERATOR

#endif  // CY_CORE_REFLECT_ANNOTATIONS_H
