// SPDX-License-Identifier: MIT
// cy/game_backend/scene_bridge.h — script behaviours on scene nodes. ABI 1.5, `add-swift-m12-gaps`.
//
// `swift-scripting`: a behaviour is "a Swift class attached to a node, receiving lifecycle
// callbacks", with the lifecycle `scene-graph-and-nodes` defines. The scene tree already drives
// that lifecycle for its own behaviours — `onEnterTree` parent first and `onReady` child first at
// the pump, `onExitTree` child first when a subtree leaves, `onEnable`/`onDisable` when effective
// enablement changes — and `cy::abi::BehaviourRuntime` owns the script instances. This is the join:
//
//   * every script behaviour type the module registered becomes a SCENE behaviour type of the same
//     name, whose callbacks forward to the runtime. So a node gets a script by the tree's own
//     `attach`, a prefab that names a script behaviour spawns with one, and a destroyed node
//     destroys its script — with no second lifecycle to keep in step with the first;
//   * `onCreate` creates the script instance, `onDestroy` destroys it, and the five tree callbacks
//     reach the instance through ABI 1.5's `CyBehaviourVTable` entries, through the vtable of the
//     generation that created it;
//   * it is the `SceneBackend` behind `node_find`, so a Swift `@Node(path)` resolves against the
//     same tree.
//
// THE SCRIPT TYPES ARE PER-INSTANCE AND HAVE NO PER-TICK CALLBACK HERE. `onFixedUpdate` and
// `onUpdate` stay the runtime's (`BehaviourRuntime::fixed_update` / `frame_update`), so registering
// a script type with the tree adds no dispatch system and changes no frame.
//
// A hot reload keeps every slot (the runtime restores instances in place), so the bindings survive
// it; call `sync_types()` after a reload to register any type the new image added.

#pragma once

#include <cy/abi/game/scene.h>
#include <cy/abi/host.h>
#include <cy/abi/module.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/scene/behaviour.h>
#include <cy/scene/node.h>

namespace cy::scene {
class SceneTree;
}  // namespace cy::scene

namespace cy::game_backend {

/// Puts script behaviours in the scene tree: each Swift behaviour type becomes a scene behaviour of
/// the same name, so the tree's pump delivers the tree callbacks, and `node_find` is answered
/// against the tree.
class ScriptSceneBridge final : public abi::game::SceneBackend {
public:
    /// Joins `tree` to `runtime`, whose host is `host`. All three are borrowed and outlive the
    /// bridge. Not thread-safe: attaching and the pump are the game thread's.
    ScriptSceneBridge(Allocator& allocator, scene::SceneTree& tree, abi::Host& host,
                      abi::BehaviourRuntime& runtime) noexcept;
    ~ScriptSceneBridge() override;

    ScriptSceneBridge(const ScriptSceneBridge&) = delete;
    ScriptSceneBridge& operator=(const ScriptSceneBridge&) = delete;
    ScriptSceneBridge(ScriptSceneBridge&&) = delete;
    ScriptSceneBridge& operator=(ScriptSceneBridge&&) = delete;

    /// Register a scene behaviour type for every script type the host's current generation has
    /// and the tree does not. Returns how many were added. AlreadyExists when a native behaviour
    /// already has a script type's name: two meanings for one name in a prefab is a content bug.
    [[nodiscard]] Expected<u32, Error> sync_types() noexcept;

    /// Attach the script behaviour `name` to `node`: its `onCreate` runs now, and `onEnterTree` and
    /// `onReady` at the next pump when the node is in the tree. Refused when the module's `create`
    /// refused, or the node already has a behaviour.
    [[nodiscard]] Status attach(scene::Node node, const char* name) noexcept;

    /// The runtime slot of the script on `entity`, or `scene::kNoBehaviourInstance`.
    [[nodiscard]] u32 slot_of(CyEntity entity) const noexcept;
    /// Scripts attached to nodes now.
    [[nodiscard]] u32 attached() const noexcept { return static_cast<u32>(attached_.size()); }

    /// `node_find`: `path` relative to the node `from`, or absolute from the root.
    CyResult find(CyEntity from, const char* path, CyEntity& out) const noexcept override;

private:
    struct TypeBinding {
        ScriptSceneBridge* bridge = nullptr;
        const char* name = "";
    };
    struct Attached {
        CyEntity entity = CY_ENTITY_NULL;
        u32 slot = 0;
    };

    static void on_create(const scene::BehaviourContext& context) noexcept;
    static void on_destroy(const scene::BehaviourContext& context) noexcept;
    static void on_enter_tree(const scene::BehaviourContext& context) noexcept;
    static void on_ready(const scene::BehaviourContext& context) noexcept;
    static void on_enable(const scene::BehaviourContext& context) noexcept;
    static void on_disable(const scene::BehaviourContext& context) noexcept;
    static void on_exit_tree(const scene::BehaviourContext& context) noexcept;
    static void forward(const scene::BehaviourContext& context,
                        abi::TreeCallback callback) noexcept;

    [[nodiscard]] Status add_type(const char* name) noexcept;
    [[nodiscard]] bool owns_type(const char* name) const noexcept;
    void created(CyEntity entity, const char* name) noexcept;
    void destroyed(CyEntity entity) noexcept;

    Allocator& allocator_;
    scene::SceneTree* tree_;
    abi::Host& host_;
    abi::BehaviourRuntime& runtime_;
    /// Individually allocated: the scene registry holds each binding's address as `user`.
    Array<TypeBinding*> types_;
    Array<Attached> attached_;
    /// Set by `on_create` when the module refused, so `attach` can report it.
    bool create_failed_ = false;
};

/// Bind `bridge` as `host.game.scene`, or unbind with null.
void bind_scene(cy::abi::Host& host, ScriptSceneBridge* bridge) noexcept;

}  // namespace cy::game_backend
