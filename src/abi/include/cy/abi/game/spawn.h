// SPDX-License-Identifier: MIT
// cy/abi/game/spawn.h — the backend behind ABI 1.3's `spawn_*` entries. `add-swift-game-api`.
//
// OWNER: implementer C (audio, spawning and time). See design.md in the change.
//
// Over `serialization-and-prefabs`' cooked `EntityTemplate` (cy/scene/serialization/spawn.h) for a
// prefab, and `cy::scene::SceneTree::load` for a scene asset, placed under a parent with a pose.
// Destruction is `SceneTree::destroy_node`, which runs exit and destroy callbacks child-first while
// the components still exist.
//
// DETERMINISM. Instantiation allocates entities in a fixed order — template index order, instance
// by instance — so the same calls in the same fixed step give the same entities on every run. The
// thunk bumps the ABI world's epoch after every successful call; the backend does not.

#pragma once

#include <cy/abi/cy_abi.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

namespace cy::abi::game {

/// Prefab and scene instantiation as ABI 1.3's `spawn_*` entries see it: resolve, place, destroy.
class SpawnBackend {
public:
    virtual ~SpawnBackend() = default;

    /// `spawn_resolve`. `may_load` is true only in CY_PHASE_NONE; otherwise an asset that is not
    /// resident is UNAVAILABLE and nothing is loaded. NOT_FOUND for an unknown path.
    [[nodiscard]] virtual CyResult resolve(const char* asset, bool may_load,
                                           CyPrefab& out_prefab) noexcept = 0;

    /// `spawn_instantiate`: `params` is whole. NOT_FOUND for a stale prefab or a dead parent.
    [[nodiscard]] virtual CyResult instantiate(CyPrefab prefab, const CySpawnParams& params,
                                               CyEntity& out_root) noexcept = 0;

    /// `spawn_instantiate_many`: `poses.size() == out_roots.size()`, checked by the thunk. All or
    /// nothing: on failure no instance of the batch remains.
    [[nodiscard]] virtual CyResult instantiate_many(CyPrefab prefab, CyEntity parent,
                                                    Span<const CyPose> poses,
                                                    Span<CyEntity> out_roots) noexcept = 0;

    /// `spawn_destroy`: NOT_FOUND for an entity that is not alive.
    [[nodiscard]] virtual CyResult destroy(CyEntity root) noexcept = 0;
};

}  // namespace cy::abi::game
