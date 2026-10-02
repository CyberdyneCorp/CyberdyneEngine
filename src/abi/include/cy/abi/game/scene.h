// SPDX-License-Identifier: MIT
// cy/abi/game/scene.h — the backend behind ABI 1.5's `node_find`. `add-swift-m12-gaps`.
//
// A node path is the scene tree's own addressing (`scene-graph-and-nodes`): `/Level/Player` from
// the root, `Camera`, `./Camera` or `../Camera` from a node. `cy_abi` cannot name the scene module
// — it is a layer below it — so the tree is reached through this backend, which
// `cy::game_backend::ScriptSceneBridge` implements over `cy::scene::SceneTree`. A Swift
// `@Node(path)` is resolved through it at `onReady`.

#pragma once

#include <cy/abi/cy_abi.h>
#include <cy/core/base/types.h>

namespace cy::abi::game {

/// The scene tree, as `node_find` sees it.
class SceneBackend {
public:
    virtual ~SceneBackend() = default;

    /// The node `path` names, relative to `from` or absolute. NOT_FOUND, reported, when it does not
    /// resolve or `from` is not a node; `out` is untouched then. `path` is non-null and non-empty.
    [[nodiscard]] virtual CyResult find(CyEntity from, const char* path,
                                        CyEntity& out) const noexcept = 0;
};

}  // namespace cy::abi::game
