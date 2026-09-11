// M9 TASK 4.6 — THE DEDICATED SERVER: HEADLESS BY LINK GRAPH, NOT BY A RUNTIME FLAG.
//
// `networking-and-replication`: "The engine SHALL support a dedicated server configuration that
// excludes client-only subsystems and content **at build and cook time, rather than disabling them
// at runtime**."
//
// A runtime flag satisfies nobody: the renderer is still linked, the shaders still packaged, and
// the first `if (!headless)` anyone forgets creates a window on a machine with no display. So the
// first thing in this file is five `__has_include` assertions, the way
// `src/gameplay/tests/test_bypass.cpp` asserts that gameplay cannot reach an input device. Adding a
// renderer, an audio server, an interface or a GPU backend to `src/networking/CMakeLists.txt` makes
// one of them reachable and stops this translation unit compiling — which is the point: the
// dependency is the thing to notice.
//
// `integration`, because the admission cases build a transport and a session.

#include "fixture.h"

#include <cy/networking/local_transport.h>
#include <cy/networking/server.h>

#if __has_include(<cy/rendering/assembly/frame_assembly.h>)
static_assert(false,
              "src/networking/ can see the renderer. A dedicated server build SHALL exclude the "
              "renderer, and the way that is made true is the dependency list in "
              "src/networking/CMakeLists.txt rather than a runtime branch. Remove the dependency; "
              "do not delete this check.");
#endif
#if __has_include(<cy/servers/audio/server.h>) || __has_include(<cy/audio/acoustics.h>)
static_assert(false,
              "src/networking/ can see client audio. A dedicated server build SHALL exclude it. "
              "Remove the dependency; do not delete this check.");
#endif
#if __has_include(<cy/ui/store.h>) || __has_include(<cy/ui/layout.h>)
static_assert(false,
              "src/networking/ can see the interface. A dedicated server build SHALL exclude UI. "
              "Remove the dependency; do not delete this check.");
#endif
#if __has_include(<cy/vfx/asset.h>)
static_assert(false,
              "src/networking/ can see VFX. A dedicated server build SHALL exclude it, and VFX is "
              "additionally one of the subsystems a lockstep session must exclude from "
              "authoritative simulation. Remove the dependency; do not delete this check.");
#endif
#if __has_include(<cy/backends/rhi/backend.h>) || __has_include(<cy/backends/rhi/device.h>)
static_assert(false,
              "src/networking/ can see a graphics backend. \"WHEN a dedicated server build is "
              "produced THEN no graphics backend SHALL be linked.\" Remove the dependency; do not "
              "delete this check.");
#endif

using namespace cy::net_test;
using cy::u32;
using cy::u64;

namespace {

constexpr PeerId kAlice = PeerId::make(1, 1);
constexpr PeerId kBob = PeerId::make(2, 1);
constexpr PeerId kCarol = PeerId::make(3, 1);

[[nodiscard]] CompatibilityScope server_scope() noexcept {
    CompatibilityScope scope;
    scope.platform = "linux";
    scope.architecture = "x86_64";
    scope.build_id = 0xC0FF'EE01ULL;
    scope.schema_set_hash = kEmptySchemaSetHash;
    scope.profile = cy::determinism::DeterminismProfile::SamePlatform;
    return scope;
}

}  // namespace

CY_TEST_CASE("networking: a server cook drops the client's assets and keeps the collision") {
    // Sizes in `u64` from the first multiplication, because the fields they initialise are
    // `u64` and an `int` product widened afterwards is the arithmetic that overflows once the
    // numbers get real.
    constexpr u64 kKilobyte = 1024;
    constexpr u64 kMegabyte = kKilobyte * 1024;
    const CookAsset assets[] = {
        {CookAsset::Kind::Texture, 4 * kMegabyte, 0},
        {CookAsset::Kind::Shader, 512 * kKilobyte, 0},
        {CookAsset::Kind::Audio, 8 * kMegabyte, 0},
        {CookAsset::Kind::VfxAsset, 256 * kKilobyte, 0},
        // "WHEN a mesh contributes collision geometry THEN the server cook SHALL retain the
        // collision representation without the render mesh."
        {CookAsset::Kind::Mesh, 16 * kMegabyte, 64 * kKilobyte},
        {CookAsset::Kind::Mesh, 2 * kMegabyte, 0},
        {CookAsset::Kind::Navigation, 1 * kMegabyte, 0},
        {CookAsset::Kind::GameplayData, 128 * kKilobyte, 0},
    };

    const CookReport report =
        apply_cook_profile(CookExclusions{}, cy::Span<const CookAsset>(assets, 8));
    CY_CHECK_EQ(report.assets_examined, 8U);
    CY_CHECK_EQ(report.excluded, 5U);
    CY_CHECK_EQ(report.retained, 3U);
    // The subset case is counted apart from a whole mesh being kept, because a report that did not
    // distinguish them would read the same either way.
    CY_CHECK_EQ(report.collision_subsets, 1U);
    CY_CHECK_EQ(report.bytes_retained, (64 * kKilobyte) + kMegabyte + (128 * kKilobyte));
    CY_CHECK_GT(report.bytes_excluded, 25 * kMegabyte);

    // "WHEN a server cook completes THEN it SHALL report what was excluded and the resulting size,
    // so accidental inclusions are visible." Excluded plus retained accounts for every byte.
    u64 total = 0;
    for (const CookAsset& asset : assets) {
        total += asset.bytes;
    }
    CY_CHECK_EQ(report.bytes_retained + report.bytes_excluded, total);

    // A profile that keeps the meshes keeps them whole, so the flags are the thing that decides
    // rather than a constant inside the function.
    CookExclusions keep_meshes;
    keep_meshes.high_resolution_meshes = false;
    const CookReport kept = apply_cook_profile(keep_meshes, cy::Span<const CookAsset>(assets, 8));
    CY_CHECK_EQ(kept.collision_subsets, 0U);
    CY_CHECK_GT(kept.bytes_retained, report.bytes_retained);
}

CY_TEST_CASE("networking: the server admits a matching peer and names what a mismatch was") {
    cy::Allocator& memory = allocator();
    LocalNetwork network(memory, 1);
    const PeerId host = network.add_host("server").value();
    LocalTransport transport(memory, network, host);

    DedicatedServer server(memory, transport, NetworkMode::Rollback, server_scope(),
                           /*maximum_peers=*/2);
    CY_CHECK_EQ(server.peer_count(), 0U);
    // Nobody has joined, so "everyone is ready" is false rather than vacuously true.
    CY_CHECK_FALSE(server.everyone_ready());

    CY_REQUIRE(server.admit(kAlice, server_scope(), 1).has_value());
    CY_CHECK_EQ(server.peer_count(), 1U);
    CY_CHECK_FALSE(server.admit(kAlice, server_scope(), 1).has_value());

    CompatibilityScope other_build = server_scope();
    other_build.build_id = 0xDEAD'BEEFULL;
    const auto refused = server.admit(kBob, other_build, 2);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(refused.error().refusal == AdmissionRefusal::Incompatible);
    CY_CHECK(refused.error().compatibility == JoinRefusal::BuildMismatch);

    CY_REQUIRE(server.admit(kBob, server_scope(), 2).has_value());
    const auto full = server.admit(kCarol, server_scope(), 3);
    CY_REQUIRE_FALSE(full.has_value());
    CY_CHECK(full.error().refusal == AdmissionRefusal::SessionFull);

    CY_CHECK_EQ(server.admissions(), u64{2});
    CY_CHECK_EQ(server.refusals(), u64{3});
}

CY_TEST_CASE("networking: play starts when everyone is loaded, not before") {
    cy::Allocator& memory = allocator();
    LocalNetwork network(memory, 1);
    const PeerId host = network.add_host("server").value();
    LocalTransport transport(memory, network, host);
    DedicatedServer server(memory, transport, NetworkMode::SnapshotAuthoritative, server_scope(),
                           4);

    CY_REQUIRE(server.admit(kAlice, server_scope(), 1).has_value());
    CY_REQUIRE(server.admit(kBob, server_scope(), 1).has_value());
    CY_CHECK_FALSE(server.everyone_ready());

    CY_REQUIRE(server.report_ready(kAlice).has_value());
    CY_CHECK_FALSE(server.everyone_ready());
    CY_REQUIRE(server.report_ready(kBob).has_value());
    CY_CHECK(server.everyone_ready());
    CY_CHECK_FALSE(server.report_ready(kCarol).has_value());

    server.drop(kBob);
    CY_CHECK_EQ(server.peer_count(), 1U);
}

CY_TEST_CASE("networking: the server's clock is given, never read") {
    cy::Allocator& memory = allocator();
    LocalNetwork network(memory, 1);
    const PeerId host = network.add_host("server").value();
    LocalTransport transport(memory, network, host);
    DedicatedServer server(memory, transport, NetworkMode::Lockstep, server_scope(), 4);

    // A headless server driven by a wall clock is a headless server whose replay does not
    // reproduce. `step()` takes the tick and the millisecond it is told about, and there is no
    // overload that reads either.
    for (u64 tick = 1; tick <= 10; ++tick) {
        server.step(tick, tick * 16);
    }
    CY_CHECK_EQ(server.steps(), u64{10});
    CY_CHECK(server.mode() == NetworkMode::Lockstep);
}
