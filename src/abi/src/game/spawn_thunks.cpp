// SPDX-License-Identifier: MIT
// The `spawn` thunks of ABI 1.3. `add-swift-game-api`.
//
// OWNER: implementer C (audio, spawning and time). See
// openspec/changes/add-swift-game-api/design.md.
//
// Each thunk takes cy/abi/game/services.h's steps in order — engine, phase, backend, pointers,
// `struct_size` — then asks the `SpawnBackend`, and after a successful STRUCTURAL call (the three
// that create or destroy entities) bumps the bound world's epoch, so every `CyBorrow` taken before
// it reads as stale. The backend never bumps it; this is the one place that does.
//
// PHASES. Resolving is a lookup and callable everywhere, but it may LOAD only in `N`: the backend
// is told so through `may_load`. Creating and destroying entities is simulation, so it is `[N F]` —
// never in a frame update, where a spawn would exist in one peer's world and not another's.

#include <cy/abi/errors.h>
#include <cy/abi/game/services.h>
#include <cy/abi/game/spawn.h>
#include <cy/abi/host.h>

#include <algorithm>
#include <cmath>

#include "thunks.h"

namespace cy::abi::game {
namespace {

/// Structural calls: no phase but a fixed step or none.
constexpr u32 kStructuralPhases = kPhaseNone | kPhaseFixed;

/// Steps 1 to 3: the engine, the phase, the backend. The bound backend, or null having reported
/// why — which the caller returns as `last_error_code()`.
[[nodiscard]] SpawnBackend* enter(CyEngine engine, u32 phases, const char* entry) noexcept {
    if (engine == nullptr) {
        (void)report(CY_RESULT_INVALID_ARGUMENT, "the engine is null");
        return nullptr;
    }
    if (require_phase(engine->game, phases, entry) != CY_RESULT_OK) {
        return nullptr;
    }
    if (engine->game.spawn == nullptr) {
        (void)report(CY_RESULT_UNAVAILABLE, "no spawn backend is bound to this engine");
    }
    return engine->game.spawn;
}

template <usize N>
[[nodiscard]] bool finite(const float (&values)[N]) noexcept {
    return std::ranges::all_of(values, [](float value) { return std::isfinite(value); });
}

[[nodiscard]] bool finite(const CyPose& pose) noexcept {
    return finite(pose.position) && finite(pose.rotation);
}

/// The last step of a structural call that succeeded: every borrow taken before it is stale.
[[nodiscard]] CyResult structural_ok(CyEngine engine) noexcept {
    if (engine->world != nullptr) {
        engine->world->bump_epoch();
    }
    clear_last_error();
    return CY_RESULT_OK;
}

}  // namespace

CyResult spawn_resolve(CyEngine engine, const char* asset, CyPrefab* out_prefab) {
    SpawnBackend* backend = enter(engine, kPhaseAny, "spawn_resolve");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (asset == nullptr || out_prefab == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "spawn_resolve needs an asset path and an output");
    }
    const bool may_load = engine->game.clock.phase == CY_PHASE_NONE;
    CyPrefab prefab = CY_PREFAB_NULL;
    if (const CyResult result = backend->resolve(asset, may_load, prefab); result != CY_RESULT_OK) {
        return result;
    }
    *out_prefab = prefab;
    clear_last_error();
    return CY_RESULT_OK;
}

CyResult spawn_instantiate(CyEngine engine, CyPrefab prefab, const CySpawnParams* params,
                           CyEntity* out_root) {
    SpawnBackend* backend = enter(engine, kStructuralPhases, "spawn_instantiate");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (params == nullptr || out_root == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "spawn_instantiate needs CySpawnParams and an output");
    }
    CySpawnParams whole{};
    if (!read_sized(*params, whole)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "spawn_instantiate: the CySpawnParams struct_size is malformed");
    }
    if (!finite(whole.pose) || !finite(whole.scale)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "spawn_instantiate: the pose and the scale must be finite");
    }
    CyEntity root = CY_ENTITY_NULL;
    if (const CyResult result = backend->instantiate(prefab, whole, root); result != CY_RESULT_OK) {
        return result;
    }
    *out_root = root;
    return structural_ok(engine);
}

CyResult spawn_instantiate_many(CyEngine engine, CyPrefab prefab, CyEntity parent,
                                const CyPose* poses, uint32_t count, CyEntity* out_roots) {
    SpawnBackend* backend = enter(engine, kStructuralPhases, "spawn_instantiate_many");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (count == 0U) {
        clear_last_error();
        return CY_RESULT_OK;
    }
    if (poses == nullptr || out_roots == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "spawn_instantiate_many needs `count` poses and room for `count` roots");
    }
    const Span<const CyPose> placements(poses, count);
    for (const CyPose& pose : placements) {
        if (!finite(pose)) {
            return report(CY_RESULT_INVALID_ARGUMENT,
                          "spawn_instantiate_many: every pose must be finite");
        }
    }
    if (const CyResult result =
            backend->instantiate_many(prefab, parent, placements, Span<CyEntity>(out_roots, count));
        result != CY_RESULT_OK) {
        return result;
    }
    return structural_ok(engine);
}

CyResult spawn_destroy(CyEngine engine, CyEntity root) {
    SpawnBackend* backend = enter(engine, kStructuralPhases, "spawn_destroy");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (const CyResult result = backend->destroy(root); result != CY_RESULT_OK) {
        return result;
    }
    return structural_ok(engine);
}

}  // namespace cy::abi::game
