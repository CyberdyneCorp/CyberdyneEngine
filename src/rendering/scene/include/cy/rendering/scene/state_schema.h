#pragma once
// The state schema for the renderer's three built-in components. Tasks 4.1.2 and 4.1.4, and M2's
// carried-forward debt 1.2 kept closed rather than reopened.
//
// M2's gate recorded that the state hash covered 4 of 17 subjects, and the reason was thirteen
// components registered by name with no `reflect::TypeInfo` behind them. `src/ecs/state_schema.h`
// and `src/scene/state_schema.h` closed those. THIS FILE IS THE PROMISE THAT M3 DOES NOT REOPEN IT:
// the renderer's components are registered by name for the same reason the scene's are (see
// components.h), so they are declared here, in the same change that introduces them, rather than
// added to a list of things a later milestone has to go back for.
//
// --- WHAT IS HASHED, AND WHY EACH ANSWER IS WHAT IT IS -------------------------------------------
//
// The classification decides participation (`determinism::participation_of`), so this table is the
// hash's coverage read straight off:
//
//   MeshRenderer
//     layer_mask, lod_bias,      Authoritative   what a designer sets and gameplay changes. A
//     visible, casts_shadow,                     divergence in whether an object is visible, or on
//     receives_shadow, two_sided                 which layer, is a divergence in the world.
//
//     mesh, material             Derived         A HANDLE'S VALUE IS ALLOCATION ORDER. It is a slot
//                                                index and a generation the render server assigns
//                                                as assets load; two runs that load the same assets
//                                                in a different order give the same mesh different
//                                                handles. Hashing one would report a divergence
//                                                between two identical worlds — the exact failure
//                                                `StateEncoding::InternedName` exists to prevent
//                                                for `cy::Name`, and there is no equivalent
//                                                encoding here because a handle has no text to
//                                                hash. What is authoritative is *which asset* is
//                                                assigned, and that lives in the asset reference
//                                                the loader read, not in this component.
//
//     local_bounds               Derived         a copy of the mesh asset's own bounds.
//
//     importance                 Presentation     computed by the renderer from screen coverage and
//                                                distance. Classified on the field in components.h,
//                                                and the classification here agrees with it — a
//                                                presentation field in the hash would make a camera
//                                                move look like a divergence.
//
//   LightSource                  Authoritative   every field: a light's colour, intensity, range,
//                                                cone and shadow flag are authored state, and a
//                                                divergence in any of them is a divergence in the
//                                                world. Physical units are hashed as the floats
//                                                they are.
//
//   Camera                       Presentation    EVERY FIELD, and this is the one that deserves the
//                                                argument. A camera is where the view is, and
//                                                `simulation-and-determinism` names "camera" in its
//                                                own list of presentation state. Hashing it would
//                                                make two clients watching one match from different
//                                                angles diverge by construction, which is precisely
//                                                the reading the firewall exists to forbid. A game
//                                                whose camera IS authoritative — a fixed-camera
//                                                puzzle where the view decides what is reachable —
//                                                declares its own authoritative component for that
//                                                and does not reuse this one.
//
// --- THE FIELD IDS -------------------------------------------------------------------------------
//
// Per subject, starting at 1, literals rather than derived from anything. They are not manifest
// identifiers — a built-in has none — and `StateField::id` only has to be stable across runs and
// unique within its subject.

#include <cy/core/base/expected.h>
#include <cy/core/determinism/state_schema.h>
#include <cy/rendering/scene/components.h>

namespace cy::rendering {

/// Declare the renderer's components to `schema`.
///
/// The third call a host makes, after `runtime::declare_reflected_components()` and beside
/// `ecs::declare_relationship_state()` and `scene::declare_scene_state()`, before
/// `StateSchema::freeze()`. Refuses when the components are not registered in the world the schema
/// is about: a schema over `kInvalidComponent` would address nothing and would report a coverage it
/// does not have.
[[nodiscard]] Status declare_render_state(determinism::StateSchema& schema,
                                          const RenderComponents& components) noexcept;

}  // namespace cy::rendering
