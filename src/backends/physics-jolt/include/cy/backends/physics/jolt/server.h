#pragma once
// The Jolt physics backend. Task 4.2.2.
//
// ================================================================================================
// WHAT CROSSES THIS HEADER, AND WHAT CANNOT
// ================================================================================================
//
// `physics` — "Backend types do not leak": "WHEN engine or game code is compiled THEN no Jolt type
// SHALL appear in any header outside the backend module". This header names two things: a factory
// returning `cy::physics::PhysicsServer*`, and a destroy function. There is no `JPH::` anywhere in
// it, no Jolt header is included, and there is nothing a consumer could reach a Jolt type through.
//
// The layer checker enforces the other direction: src/backends/ is layer 3 and this is the only
// root a Jolt include may appear under. The two halves together are why the requirement is a
// property of the build rather than a review note.
//
// ================================================================================================
// THE JOB SYSTEM
// ================================================================================================
//
// `physics` — "One job system": "WHEN physics steps THEN its internal parallelism SHALL run on
// engine job workers, so physics and other work share one thread pool and one scheduler".
//
// `create_server` takes an optional `cy::jobs::JobSystem*`. When one is given and running, Jolt's
// internal parallelism is dispatched onto it through a bridge (`jobs.cpp`) and
// `Capabilities::uses_engine_jobs` is true. When none is given the backend runs Jolt's work on the
// calling thread and says so through the same flag — which is the honest answer for a headless test
// that never started a scheduler, and is never silently a second thread pool.

#include <cy/core/memory/allocator.h>
#include <cy/servers/physics/server.h>

namespace cy::jobs {
class JobSystem;
}

namespace cy::physics::jolt {

/// The backend's name, as `backend_name()` reports it and as a configuration selects it.
inline constexpr const char* kBackendName = "jolt";

/// Construct the Jolt backend. The caller owns it.
///
/// `jobs` may be null; see the header comment. Jolt's global state — its allocator hook, its type
/// factory and its shape registry — is initialised on the first server and torn down with the last,
/// so two servers in one process are legal and the second does not reinitialise the first's.
[[nodiscard]] Expected<PhysicsServer*, Error> create_server(Allocator& allocator,
                                                            cy::jobs::JobSystem* jobs) noexcept;

void destroy_server(PhysicsServer* server, Allocator& allocator) noexcept;

}  // namespace cy::physics::jolt
