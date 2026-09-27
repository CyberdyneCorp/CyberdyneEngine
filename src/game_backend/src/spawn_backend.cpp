// SPDX-License-Identifier: MIT
// The `spawn` adapter: `cy::abi::game::SpawnBackend` over `cy::scene::SceneTree`.
// `add-swift-game-api`.
//
// OWNER: implementer C. See openspec/changes/add-swift-game-api/design.md and the adapter's header.
//
// The thunks have already checked the phase (structural calls only in N and F), the pointers, the
// `struct_size` and that every pose is finite. What is left is the domain: which prefabs exist and
// are resident, whether the parent is alive, and making a batch all-or-nothing.

#include <cy/game_backend/spawn_backend.h>

#include <cy/abi/errors.h>
#include <cy/core/math/quat.h>
#include <cy/scene/tree.h>

namespace cy::game_backend {
namespace {

[[nodiscard]] CyResult domain(ErrorCode code, const char* message) noexcept {
    return abi::report(abi::to_result(code), message);
}

/// A single rooted tree in topological order: node 0 is the only node without a parent, and every
/// other node's parent precedes it.
[[nodiscard]] bool well_formed(const scene::SceneDescription& description) noexcept {
    if (description.nodes.empty() || description.nodes[0].parent != scene::NodeDesc::kNoParent) {
        return false;
    }
    for (usize index = 1; index < description.nodes.size(); ++index) {
        if (description.nodes[index].parent >= index) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] CyPrefab handle_of(u32 generation, usize index) noexcept {
    return (static_cast<u64>(generation) << 32U) | static_cast<u64>(index);
}

}  // namespace

Transform placement_of(const CyPose& pose, const float (&scale)[3]) noexcept {
    Transform placement;
    placement.translation = Vec3{pose.position[0], pose.position[1], pose.position[2]};
    const Quat rotation{pose.rotation[0], pose.rotation[1], pose.rotation[2], pose.rotation[3]};
    const bool zero_rotation =
        rotation.x == 0.0F && rotation.y == 0.0F && rotation.z == 0.0F && rotation.w == 0.0F;
    placement.rotation = zero_rotation ? Quat::identity() : normalize(rotation);
    const bool zero_scale = scale[0] == 0.0F && scale[1] == 0.0F && scale[2] == 0.0F;
    placement.scale = zero_scale ? Vec3{1.0F, 1.0F, 1.0F} : Vec3{scale[0], scale[1], scale[2]};
    return placement;
}

SpawnAdapter::SpawnAdapter(scene::SceneTree& tree, Allocator& allocator) noexcept
    : tree_(tree), allocator_(allocator), prefabs_(allocator) {}

// --- Registration --------------------------------------------------------------------------------

Status SpawnAdapter::add(const char* path, Prefab prefab) noexcept {
    if (path == nullptr || path[0] == '\0') {
        return fail(ErrorCode::InvalidArgument, "a prefab needs a content path");
    }
    prefab.path = Name::intern(path);
    for (const Prefab& existing : prefabs_) {
        if (existing.path == prefab.path) {
            return fail(ErrorCode::AlreadyExists, "a prefab is already registered at that path");
        }
    }
    return prefabs_.push_back(prefab);
}

Status SpawnAdapter::add_prefab(const char* path,
                                const scene::SceneDescription& description) noexcept {
    if (!well_formed(description)) {
        return fail(ErrorCode::InvalidArgument,
                    "a prefab is one rooted tree: node 0 has no parent, and every other node's "
                    "parent precedes it");
    }
    Prefab prefab;
    prefab.description = description;
    prefab.resident = true;
    return add(path, prefab);
}

Status SpawnAdapter::add_loadable(const char* path, Loader loader, void* user) noexcept {
    if (loader == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a loadable prefab needs a loader");
    }
    Prefab prefab;
    prefab.loader = loader;
    prefab.user = user;
    return add(path, prefab);
}

void SpawnAdapter::clear() noexcept {
    prefabs_.clear();
    ++generation_;
}

// --- Resolving -----------------------------------------------------------------------------------

CyResult SpawnAdapter::load(Prefab& prefab) noexcept {
    scene::SceneDescription loaded;
    if (const Status status = prefab.loader(prefab.user, prefab.path.c_str(), loaded); !status) {
        return abi::report(status.error());
    }
    if (!well_formed(loaded)) {
        return domain(ErrorCode::InvalidArgument,
                      "spawn_resolve: the loaded prefab is not one rooted tree in order");
    }
    prefab.description = loaded;
    prefab.resident = true;
    return CY_RESULT_OK;
}

CyResult SpawnAdapter::resolve(const char* asset, bool may_load, CyPrefab& out_prefab) noexcept {
    const Name wanted = Name::find(asset);
    for (usize index = 0; !wanted.is_empty() && index < prefabs_.size(); ++index) {
        Prefab& prefab = prefabs_[index];
        if (prefab.path != wanted) {
            continue;
        }
        if (!prefab.resident) {
            if (!may_load) {
                return domain(ErrorCode::Unavailable,
                              "spawn_resolve: the prefab is not resident; resolve it in no phase "
                              "(module initialisation) to load it");
            }
            if (const CyResult loaded = load(prefab); loaded != CY_RESULT_OK) {
                return loaded;
            }
        }
        out_prefab = handle_of(generation_, index);
        return CY_RESULT_OK;
    }
    return domain(ErrorCode::NotFound, "spawn_resolve: no prefab at that path");
}

const scene::SceneDescription* SpawnAdapter::find(CyPrefab handle) const noexcept {
    const auto generation = static_cast<u32>(handle >> 32U);
    const auto index = static_cast<u32>(handle);
    if (handle == CY_PREFAB_NULL || generation != generation_ || index >= prefabs_.size() ||
        !prefabs_[index].resident) {
        (void)domain(ErrorCode::NotFound, "the prefab is null or stale");
        return nullptr;
    }
    return &prefabs_[index].description;
}

CyResult SpawnAdapter::parent_node(CyEntity parent, scene::Node& out) noexcept {
    // No parent attaches to the tree's root, so a spawned unit is in the tree and gets its
    // `onEnterTree` — the convention `gameplay::SpawnService` already follows.
    out = parent == CY_ENTITY_NULL ? tree_.root() : tree_.node(abi::from_abi(parent));
    if (!out.valid()) {
        return domain(ErrorCode::NotFound, "the spawn parent is not a live node");
    }
    return CY_RESULT_OK;
}

// --- Instantiating -------------------------------------------------------------------------------

Status SpawnAdapter::create_nodes(const scene::SceneDescription& description, scene::Node parent,
                                  const Transform& placement,
                                  Array<ecs::Entity>& created) noexcept {
    for (usize index = 0; index < description.nodes.size(); ++index) {
        const scene::NodeDesc& desc = description.nodes[index];
        const scene::Node under = index == 0 ? parent : tree_.node(created[desc.parent]);
        // `create_node` reads a dead parent as "no parent" and would leave the node detached. A
        // parent can die here — a behaviour's `create` may destroy its own node — so check.
        if (!under.valid()) {
            return fail(ErrorCode::NotFound,
                        "a node of the instance was destroyed while the instance was created");
        }
        Expected<scene::Node, Error> node = tree_.create_node(desc.name, under, desc.node_template);
        if (!node) {
            return make_unexpected(node.error());
        }
        if (Status pushed = created.push_back(node->entity()); !pushed) {
            return pushed;
        }
        const Transform local =
            index == 0 ? placement * desc.local_transform : desc.local_transform;
        if (Status placed = node->set_local_transform(local); !placed) {
            return placed;
        }
        if (!desc.visible) {
            if (Status hidden = node->set_visible(false); !hidden) {
                return hidden;
            }
        }
        if (!desc.enabled) {
            if (Status disabled = node->set_enabled(false); !disabled) {
                return disabled;
            }
        }
        if (!desc.behaviour.is_empty()) {
            const scene::BehaviourTypeId type = tree_.behaviours().find(desc.behaviour);
            if (type == scene::kInvalidBehaviour) {
                return fail(ErrorCode::NotFound, "the prefab names a behaviour not registered");
            }
            if (Status attached = tree_.behaviours().attach(tree_, *node, type); !attached) {
                return attached;
            }
        }
    }
    return ok();
}

CyResult SpawnAdapter::instantiate_one(const scene::SceneDescription& description,
                                       scene::Node parent, const Transform& placement,
                                       ecs::Entity& out_root) noexcept {
    // Local rather than a member: a behaviour's `create` may itself spawn, re-entering this.
    Array<ecs::Entity> created(allocator_);
    if (Status made = create_nodes(description, parent, placement, created); !made) {
        // Every node hangs under node 0, so destroying it removes whatever was made.
        if (!created.empty()) {
            (void)tree_.destroy_node(tree_.node(created[0]));
        }
        return abi::report(made.error());
    }
    out_root = created[0];
    return CY_RESULT_OK;
}

CyResult SpawnAdapter::instantiate(CyPrefab prefab, const CySpawnParams& params,
                                   CyEntity& out_root) noexcept {
    const scene::SceneDescription* found = find(prefab);
    if (found == nullptr) {
        return abi::last_error_code();
    }
    // A copy — a name and a span — so a behaviour's `create` that registers a prefab cannot move
    // the description out from under the instantiation.
    const scene::SceneDescription description = *found;
    scene::Node parent;
    if (const CyResult located = parent_node(params.parent, parent); located != CY_RESULT_OK) {
        return located;
    }
    ecs::Entity root;
    if (const CyResult made =
            instantiate_one(description, parent, placement_of(params.pose, params.scale), root);
        made != CY_RESULT_OK) {
        return made;
    }
    out_root = abi::to_abi(root);
    return CY_RESULT_OK;
}

void SpawnAdapter::roll_back(Span<const ecs::Entity> roots) noexcept {
    for (usize index = roots.size(); index > 0; --index) {
        (void)tree_.destroy_node(tree_.node(roots[index - 1]));
    }
}

CyResult SpawnAdapter::instantiate_many(CyPrefab prefab, CyEntity parent, Span<const CyPose> poses,
                                        Span<CyEntity> out_roots) noexcept {
    const scene::SceneDescription* found = find(prefab);
    if (found == nullptr) {
        return abi::last_error_code();
    }
    // A copy — a name and a span — so a behaviour's `create` that registers a prefab cannot move
    // the description out from under the instantiation.
    const scene::SceneDescription description = *found;
    scene::Node under;
    if (const CyResult located = parent_node(parent, under); located != CY_RESULT_OK) {
        return located;
    }
    Array<ecs::Entity> batch(allocator_);
    if (Status reserved = batch.reserve(poses.size()); !reserved) {
        return abi::report(reserved.error());
    }
    constexpr float kUnscaled[3] = {0.0F, 0.0F, 0.0F};
    for (const CyPose& pose : poses) {
        ecs::Entity root;
        const CyResult made =
            instantiate_one(description, under, placement_of(pose, kUnscaled), root);
        if (made != CY_RESULT_OK) {
            roll_back(batch.span());  // all or nothing
            return made;
        }
        (void)batch.push_back(root);  // reserved above, so this cannot fail
    }
    // Written only now, so a caller's buffer is untouched by a batch that failed.
    for (usize index = 0; index < batch.size(); ++index) {
        out_roots[index] = abi::to_abi(batch[index]);
    }
    return CY_RESULT_OK;
}

CyResult SpawnAdapter::destroy(CyEntity root) noexcept {
    const scene::Node node = tree_.node(abi::from_abi(root));
    if (root == CY_ENTITY_NULL || !node.valid()) {
        return domain(ErrorCode::NotFound, "spawn_destroy: the entity is not a live node");
    }
    if (node.entity() == tree_.root().entity()) {
        return domain(ErrorCode::InvalidArgument, "spawn_destroy: the tree's root is not a spawn");
    }
    if (const Status destroyed = tree_.destroy_node(node); !destroyed) {
        return abi::report(destroyed.error());
    }
    return CY_RESULT_OK;
}

void bind(cy::abi::Host& host, SpawnAdapter* adapter) noexcept {
    host.game.spawn = adapter;
}

}  // namespace cy::game_backend
