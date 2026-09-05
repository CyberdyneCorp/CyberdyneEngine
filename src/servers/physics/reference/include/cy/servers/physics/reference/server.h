#pragma once
// The reference physics backend: the whole `PhysicsServer` interface with no third-party library
// behind it. Task 4.2.1, design.md §4.
//
// ================================================================================================
// WHY THIS EXISTS, AND WHY IT IS RETAINED
// ================================================================================================
//
// `thirdparty-dependencies` and design.md §4: "PhysicsServer and the audio driver layer are defined
// and exercised by a trivial implementation BEFORE Jolt and miniaudio are linked... The retained
// trivial implementation is not ceremony: it is what proves at every build that the interface does
// not leak the library."
//
// It was written first. Every method of `PhysicsServer` was designed against it, and the interface
// is the shape it is because nothing was allowed to lean on a solver that did not exist yet. It
// stays for three reasons, each of which has cost this project time when it was missing elsewhere:
//
//   1. it is the last link in the backend fallback chain, so `physics.reference` runs on a machine,
//      or in a configuration, that has no Jolt — `-D CY_PHYSICS=OFF` still simulates, still steps
//      on the simulation clock and still runs the determinism suite;
//   2. it is what makes `CharacterController`'s suite a CONFORMANCE test: the controller is engine
//      code over `PhysicsServer`'s queries, and running its cases over two independent backends is
//      how "no gameplay code changes when the backend changes" is checked instead of asserted;
//   3. an interface that only one implementation has ever satisfied is a wrapper. This one has two.
//
// ================================================================================================
// WHAT IT SIMULATES, STATED PRECISELY, BECAUSE THE HONEST LIMITS ARE THE POINT
// ================================================================================================
//
// IT DOES:  gravity, damping, per-axis degree-of-freedom locks, sleeping, kinematic and static
//           bodies, the shape cache, collision filtering through the layer/mask rule and the
//           project matrix, per-pair ignore lists, contact and trigger ENTER/STAY/EXIT events,
//           raycasts, shape casts, overlaps and closest-point queries, the state hash, and debug
//           draw.
//
// IT DOES NOT: resolve contacts. Two solid bodies pass through each other, and
//           `Capabilities::contact_resolution` is FALSE so that a caller is told rather than
//           surprised. It also does not implement constraints, soft bodies or vehicles, and
//           creating one returns `Unsupported` naming this backend — which is `physics`'
//           "Unsupported feature" scenario, and the reason that scenario has a test at all.
//
// IT COLLIDES BOUNDING VOLUMES, NOT SHAPES. A sphere is exact, a plane is exact, a box is exact
// while it is axis-aligned; everything else — capsule, cylinder, hull, mesh, height field — is its
// local bounding box, transformed. That is a REFERENCE backend's job: to be obviously correct about
// the interface rather than approximately correct about geometry. Where the distinction matters,
// the character controller's suite runs the same case over Jolt as well, and the two must agree
// about the behaviour the requirement names — grounded or not, stepped or blocked — not about the
// millimetre.

#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/servers/physics/server.h>

namespace cy::physics::reference {

/// Construct the reference backend. The caller owns it.
///
/// A factory rather than an exported class, so the backend's storage types stay private to
/// reference/src/ and a consumer links against `PhysicsServer` alone — which is the same
/// arrangement that keeps `cy::rhi::create_null_device()` from exposing its device's internals.
[[nodiscard]] Expected<PhysicsServer*, Error> create_server(Allocator& allocator) noexcept;

/// Destroy a server `create_server` returned.
void destroy_server(PhysicsServer* server, Allocator& allocator) noexcept;

/// The backend's name, as `backend_name()` reports it and as a configuration selects it.
inline constexpr const char* kBackendName = "reference";

}  // namespace cy::physics::reference
