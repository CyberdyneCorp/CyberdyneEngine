// SPDX-License-Identifier: MIT
// ABI 1.3's `spawn_*` thunks, against a fake `SpawnBackend`. `add-swift-game-api`.
//
// OWNER: implementer C. What is proven here is the boundary: the `[N F]` phase list of the three
// structural entries, `may_load` only in N, the `struct_size` handling of `CySpawnParams`, the
// argument checks, and the epoch bump after — and only after — a successful structural call. What a
// real scene tree makes of an instance is `integration.game_backend_spawn`'s.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/game/services.h>
#include <cy/abi/game/spawn.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/world.h>
#include <cy/test/test.h>

#include <cstddef>
#include <limits>
#include <string>

namespace {

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Scripting);
}

constexpr CyPrefab kTank = 0x1'0000'0000ULL;
constexpr CyEntity kRoot = 0x1'0000'0010ULL;

/// Knows one prefab, "units/tank", resident only after a load; issues roots from `kRoot` upward.
class FakeSpawn final : public cy::abi::game::SpawnBackend {
public:
    CyResult resolve(const char* asset, bool may_load, CyPrefab& out_prefab) noexcept override {
        ++calls;
        last_may_load = may_load;
        if (std::string(asset) != "units/tank") {
            return cy::abi::report(CY_RESULT_NOT_FOUND, "no such prefab");
        }
        if (!resident && !may_load) {
            return cy::abi::report(CY_RESULT_UNAVAILABLE, "not resident");
        }
        resident = true;
        out_prefab = kTank;
        return CY_RESULT_OK;
    }
    CyResult instantiate(CyPrefab prefab, const CySpawnParams& params,
                         CyEntity& out_root) noexcept override {
        ++calls;
        last_params = params;
        if (prefab != kTank) {
            return cy::abi::report(CY_RESULT_NOT_FOUND, "stale prefab");
        }
        out_root = kRoot + (issued++);
        return CY_RESULT_OK;
    }
    CyResult instantiate_many(CyPrefab prefab, CyEntity parent, cy::Span<const CyPose> poses,
                              cy::Span<CyEntity> out_roots) noexcept override {
        ++calls;
        last_parent = parent;
        last_count = poses.size();
        if (prefab != kTank || poses.size() != out_roots.size()) {
            return cy::abi::report(CY_RESULT_NOT_FOUND, "stale prefab");
        }
        for (CyEntity& root : out_roots) {
            root = kRoot + (issued++);
        }
        return CY_RESULT_OK;
    }
    CyResult destroy(CyEntity root) noexcept override {
        ++calls;
        if (root < kRoot || root >= kRoot + issued) {
            return cy::abi::report(CY_RESULT_NOT_FOUND, "not alive");
        }
        return CY_RESULT_OK;
    }

    int calls = 0;
    bool resident = false;
    bool last_may_load = false;
    CySpawnParams last_params{};
    CyEntity last_parent = CY_ENTITY_NULL;
    cy::usize last_count = 0;
    CyEntity issued = 0;
};

/// A host with a bound world — the epoch lives on it — and the fake backend.
struct SpawnHost {
    cy::ecs::World ecs{allocator()};
    cy::abi::World world{allocator(), ecs};
    cy::abi::Host host{allocator()};
    FakeSpawn spawn;

    SpawnHost() {
        CY_REQUIRE(ecs.initialize().has_value());
        host.bind_world(&world);
        host.game.spawn = &spawn;
    }
};

CySpawnParams params_at(float x) noexcept {
    CySpawnParams params{};
    params.struct_size = sizeof(CySpawnParams);
    params.pose.position[0] = x;
    params.pose.rotation[3] = 1.0F;
    params.scale[0] = 2.0F;
    params.scale[1] = 2.0F;
    params.scale[2] = 2.0F;
    return params;
}

}  // namespace

CY_TEST_CASE("spawn: resolve may load only in N; in F and U an absent asset is UNAVAILABLE") {
    SpawnHost fixture;
    CyPrefab prefab = CY_PREFAB_NULL;

    fixture.host.game.clock.phase = CY_PHASE_FIXED_UPDATE;
    CY_CHECK_EQ(table().spawn_resolve(&fixture.host, "units/tank", &prefab), CY_RESULT_UNAVAILABLE);
    CY_CHECK_FALSE(fixture.spawn.last_may_load);
    CY_CHECK_EQ(prefab, CY_PREFAB_NULL);

    fixture.host.game.clock.phase = CY_PHASE_NONE;
    CY_REQUIRE_EQ(table().spawn_resolve(&fixture.host, "units/tank", &prefab), CY_RESULT_OK);
    CY_CHECK(fixture.spawn.last_may_load);
    CY_CHECK_EQ(prefab, kTank);

    // Resident now, so the frame may resolve it.
    fixture.host.game.clock.phase = CY_PHASE_FRAME_UPDATE;
    prefab = CY_PREFAB_NULL;
    CY_CHECK_EQ(table().spawn_resolve(&fixture.host, "units/tank", &prefab), CY_RESULT_OK);
    CY_CHECK_FALSE(fixture.spawn.last_may_load);
    CY_CHECK_EQ(prefab, kTank);

    CY_CHECK_EQ(table().spawn_resolve(&fixture.host, "units/none", &prefab), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(table().spawn_resolve(&fixture.host, nullptr, &prefab), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(table().spawn_resolve(&fixture.host, "units/tank", nullptr),
                CY_RESULT_INVALID_ARGUMENT);
    cy::abi::clear_last_error();
}

CY_TEST_CASE("spawn: instantiate returns the root and bumps the epoch; a failure bumps nothing") {
    SpawnHost fixture;
    const cy::u64 before = fixture.world.epoch;
    const CySpawnParams params = params_at(4.0F);
    CyEntity root = CY_ENTITY_NULL;
    CY_REQUIRE_EQ(table().spawn_instantiate(&fixture.host, kTank, &params, &root), CY_RESULT_OK);
    CY_CHECK_EQ(root, kRoot);
    CY_CHECK_EQ(fixture.world.epoch, before + 1);
    CY_CHECK_EQ(fixture.spawn.last_params.pose.position[0], 4.0F);
    CY_CHECK_EQ(fixture.spawn.last_params.scale[2], 2.0F);

    CyEntity untouched = 5;
    CY_CHECK_EQ(table().spawn_instantiate(&fixture.host, 123, &params, &untouched),
                CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(untouched, 5U);
    CY_CHECK_EQ(fixture.world.epoch, before + 1);
    cy::abi::clear_last_error();
}

CY_TEST_CASE("spawn: an older caller's short CySpawnParams reaches the backend zero-filled") {
    SpawnHost fixture;
    CySpawnParams params = params_at(4.0F);
    params.struct_size = static_cast<uint32_t>(offsetof(CySpawnParams, scale));
    CyEntity root = CY_ENTITY_NULL;
    CY_REQUIRE_EQ(table().spawn_instantiate(&fixture.host, kTank, &params, &root), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.spawn.last_params.struct_size, sizeof(CySpawnParams));
    CY_CHECK_EQ(fixture.spawn.last_params.pose.position[0], 4.0F);
    CY_CHECK_EQ(fixture.spawn.last_params.scale[0], 0.0F);  // "all zero is read as one"

    params.struct_size = 1;
    CY_CHECK_EQ(table().spawn_instantiate(&fixture.host, kTank, &params, &root),
                CY_RESULT_INVALID_ARGUMENT);
    cy::abi::clear_last_error();
}

CY_TEST_CASE("spawn: instantiate refuses null pointers and a non-finite pose or scale") {
    SpawnHost fixture;
    CySpawnParams params = params_at(0.0F);
    CyEntity root = CY_ENTITY_NULL;
    CY_CHECK_EQ(table().spawn_instantiate(&fixture.host, kTank, nullptr, &root),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(table().spawn_instantiate(&fixture.host, kTank, &params, nullptr),
                CY_RESULT_INVALID_ARGUMENT);
    params.pose.rotation[1] = std::numeric_limits<float>::quiet_NaN();
    CY_CHECK_EQ(table().spawn_instantiate(&fixture.host, kTank, &params, &root),
                CY_RESULT_INVALID_ARGUMENT);
    params = params_at(0.0F);
    params.scale[2] = std::numeric_limits<float>::infinity();
    CY_CHECK_EQ(table().spawn_instantiate(&fixture.host, kTank, &params, &root),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(fixture.spawn.calls, 0);
    cy::abi::clear_last_error();
}

CY_TEST_CASE("spawn: the structural entries refuse a frame update, in every build") {
    SpawnHost fixture;
    fixture.host.game.clock.phase = CY_PHASE_FRAME_UPDATE;
    const CySpawnParams params = params_at(0.0F);
    const CyPose pose{};
    CyEntity root = CY_ENTITY_NULL;
    CY_CHECK_EQ(table().spawn_instantiate(&fixture.host, kTank, &params, &root),
                CY_RESULT_PERMISSION_DENIED);
    CY_CHECK_EQ(
        table().spawn_instantiate_many(&fixture.host, kTank, CY_ENTITY_NULL, &pose, 1, &root),
        CY_RESULT_PERMISSION_DENIED);
    CY_CHECK_EQ(table().spawn_destroy(&fixture.host, kRoot), CY_RESULT_PERMISSION_DENIED);
    CY_CHECK_EQ(fixture.spawn.calls, 0);

    fixture.host.game.clock.phase = CY_PHASE_FIXED_UPDATE;
    CY_CHECK_EQ(table().spawn_instantiate(&fixture.host, kTank, &params, &root), CY_RESULT_OK);
    cy::abi::clear_last_error();
}

CY_TEST_CASE("spawn: a batch passes every pose and fills every root, and bumps the epoch once") {
    SpawnHost fixture;
    const cy::u64 before = fixture.world.epoch;
    CyPose poses[3] = {};
    CyEntity roots[3] = {};
    CY_REQUIRE_EQ(table().spawn_instantiate_many(&fixture.host, kTank, kRoot, poses, 3, roots),
                  CY_RESULT_OK);
    CY_CHECK_EQ(fixture.spawn.last_count, 3U);
    CY_CHECK_EQ(fixture.spawn.last_parent, kRoot);
    CY_CHECK_EQ(roots[0], kRoot);
    CY_CHECK_EQ(roots[2], kRoot + 2);
    CY_CHECK_EQ(fixture.world.epoch, before + 1);

    // An empty batch is nothing, successfully, and needs no buffers.
    CY_CHECK_EQ(table().spawn_instantiate_many(&fixture.host, kTank, kRoot, nullptr, 0, nullptr),
                CY_RESULT_OK);
    CY_CHECK_EQ(fixture.world.epoch, before + 1);

    CY_CHECK_EQ(table().spawn_instantiate_many(&fixture.host, kTank, kRoot, nullptr, 2, roots),
                CY_RESULT_INVALID_ARGUMENT);
    poses[1].position[0] = std::numeric_limits<float>::infinity();
    CY_CHECK_EQ(table().spawn_instantiate_many(&fixture.host, kTank, kRoot, poses, 3, roots),
                CY_RESULT_INVALID_ARGUMENT);
    cy::abi::clear_last_error();
}

CY_TEST_CASE("spawn: destroy bumps the epoch; a dead entity is NOT_FOUND and bumps nothing") {
    SpawnHost fixture;
    const CySpawnParams params = params_at(0.0F);
    CyEntity root = CY_ENTITY_NULL;
    CY_REQUIRE_EQ(table().spawn_instantiate(&fixture.host, kTank, &params, &root), CY_RESULT_OK);
    const cy::u64 before = fixture.world.epoch;
    CY_CHECK_EQ(table().spawn_destroy(&fixture.host, root), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.world.epoch, before + 1);
    CY_CHECK_EQ(table().spawn_destroy(&fixture.host, 0xDEAD), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(fixture.world.epoch, before + 1);
    cy::abi::clear_last_error();
}

CY_TEST_CASE("spawn: with no backend every entry is UNAVAILABLE") {
    cy::abi::Host host(allocator());
    const CySpawnParams params = params_at(0.0F);
    CyPrefab prefab = CY_PREFAB_NULL;
    CyEntity root = CY_ENTITY_NULL;
    CY_CHECK_EQ(table().spawn_resolve(&host, "units/tank", &prefab), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(table().spawn_instantiate(&host, kTank, &params, &root), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(table().spawn_instantiate_many(&host, kTank, CY_ENTITY_NULL, nullptr, 0, nullptr),
                CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(table().spawn_destroy(&host, kRoot), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(table().spawn_destroy(nullptr, kRoot), CY_RESULT_INVALID_ARGUMENT);
    cy::abi::clear_last_error();
}
