// SPDX-License-Identifier: MIT
// cy/game_backend/spawn_backend.h — the `spawn` adapter behind ABI 1.3. `add-swift-game-api`.
//
// OWNER: implementer C. Implements cy/abi/game/spawn.h over `cy::scene::SceneTree`, to the contract
// in design.md.
//
// WHAT A PREFAB IS HERE. A `SceneDescription`: nodes in topological order, node 0 the instance's
// root and the only node without a parent. That is what `SceneTree::load` instantiates for a scene
// asset, and the adapter instantiates it the same way — template, local transform, visibility,
// enabled flag, behaviour — but under a caller's parent and pose, as many times as asked. Aliases
// are NOT applied: an alias names one node in the world, and a prefab is spawned many times.
//
// RESIDENCY. A prefab is registered either resident (`add_prefab`) or with a loader
// (`add_loadable`). `spawn_resolve` runs the loader only when the thunk says it may — in
// CY_PHASE_NONE — and answers UNAVAILABLE otherwise, so a fixed step never stalls on a load.
//
// HANDLES. `CyPrefab` is the registration's index and the adapter's generation. `clear()` — the
// level that issued the handles is gone — bumps the generation, so every earlier handle answers
// NOT_FOUND rather than naming whatever is registered at its index next.
//
// DETERMINISM. Nodes are created in description order, instance by instance, through
// `SceneTree::create_node`, which allocates entities in call order. The same calls in the same
// fixed step therefore give the same entities on every run. The thunk bumps the ABI world's epoch
// after a successful call; the adapter does not.

#pragma once

#include <cy/abi/game/spawn.h>
#include <cy/abi/host.h>
#include <cy/core/base/expected.h>
#include <cy/core/math/transform.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/ecs/entity.h>
#include <cy/scene/node.h>
#include <cy/scene/scene.h>

namespace cy::scene {
class SceneTree;
}  // namespace cy::scene

namespace cy::game_backend {

/// Implements `cy::abi::game::SpawnBackend`.
class SpawnAdapter final : public abi::game::SpawnBackend {
public:
    /// Fill `out` with the prefab stored at `path`. `out.nodes` must stay valid until `clear()`.
    using Loader = Status (*)(void* user, const char* path, scene::SceneDescription& out) noexcept;

    /// `tree` is borrowed and outlives the adapter.
    SpawnAdapter(scene::SceneTree& tree, Allocator& allocator) noexcept;

    SpawnAdapter(const SpawnAdapter&) = delete;
    SpawnAdapter& operator=(const SpawnAdapter&) = delete;
    SpawnAdapter(SpawnAdapter&&) = delete;
    SpawnAdapter& operator=(SpawnAdapter&&) = delete;
    ~SpawnAdapter() override = default;

    /// Register a resident prefab under its content path. `description.nodes` is borrowed and must
    /// stay valid until `clear()`. Refused when the description is not a single rooted tree in
    /// topological order, or the path is already registered.
    [[nodiscard]] Status add_prefab(const char* path,
                                    const scene::SceneDescription& description) noexcept;

    /// Register a prefab that `loader` produces the first time it is resolved in CY_PHASE_NONE.
    [[nodiscard]] Status add_loadable(const char* path, Loader loader, void* user) noexcept;

    /// Forget every prefab. Every `CyPrefab` issued so far answers NOT_FOUND afterwards.
    void clear() noexcept;

    // --- SpawnBackend ---------------------------------------------------------------------------
    [[nodiscard]] CyResult resolve(const char* asset, bool may_load,
                                   CyPrefab& out_prefab) noexcept override;
    [[nodiscard]] CyResult instantiate(CyPrefab prefab, const CySpawnParams& params,
                                       CyEntity& out_root) noexcept override;
    [[nodiscard]] CyResult instantiate_many(CyPrefab prefab, CyEntity parent,
                                            Span<const CyPose> poses,
                                            Span<CyEntity> out_roots) noexcept override;
    [[nodiscard]] CyResult destroy(CyEntity root) noexcept override;

private:
    struct Prefab {
        Name path;
        scene::SceneDescription description;
        Loader loader = nullptr;
        void* user = nullptr;
        bool resident = false;
    };

    [[nodiscard]] Status add(const char* path, Prefab prefab) noexcept;
    [[nodiscard]] static CyResult load(Prefab& prefab) noexcept;
    /// The resident prefab `handle` names, or null having reported NOT_FOUND.
    [[nodiscard]] const scene::SceneDescription* find(CyPrefab handle) const noexcept;
    [[nodiscard]] CyResult parent_node(CyEntity parent, scene::Node& out) noexcept;
    /// One instance under `parent` at `placement`; on failure nothing of it remains.
    [[nodiscard]] CyResult instantiate_one(const scene::SceneDescription& description,
                                           scene::Node parent, const Transform& placement,
                                           ecs::Entity& out_root) noexcept;
    [[nodiscard]] Status create_nodes(const scene::SceneDescription& description,
                                      scene::Node parent, const Transform& placement,
                                      Array<ecs::Entity>& created) noexcept;
    /// Destroy the roots of a batch that failed part-way, newest first.
    void roll_back(Span<const ecs::Entity> roots) noexcept;

    scene::SceneTree& tree_;
    Allocator& allocator_;
    Array<Prefab> prefabs_;
    u32 generation_ = 1;
};

/// The transform a `CyPose` and a scale describe, reading an all-zero quaternion as the identity
/// and an all-zero scale as one — so a zeroed `CySpawnParams` places at the origin, unscaled.
[[nodiscard]] Transform placement_of(const CyPose& pose, const float (&scale)[3]) noexcept;

/// Bind `adapter` as `host`'s spawn backend (`host.game.spawn`), or unbind with null. The one place
/// an embedder wires the spawn service; the adapter must outlive the binding.
void bind(cy::abi::Host& host, SpawnAdapter* adapter) noexcept;

}  // namespace cy::game_backend
