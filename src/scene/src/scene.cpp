// Scenes: loading a description into the tree, budgeted instantiation, and unloading. Task 3.1.9.

#include <cy/scene/scene.h>

#include <cy/ecs/query.h>
#include <cy/scene/tree.h>

#include <algorithm>

namespace cy::scene {

const char* scene_status_name(SceneStatus status) noexcept {
    switch (status) {
        case SceneStatus::Loading:
            return "loading";
        case SceneStatus::Active:
            return "active";
        case SceneStatus::Unloaded:
            return "unloaded";
    }
    return "unknown";
}

SceneTree::SceneRecord* SceneTree::find_scene(SceneId scene) noexcept {
    for (SceneRecord* record : scenes_) {
        if (record->id == scene) {
            return record;
        }
    }
    return nullptr;
}

const SceneTree::SceneRecord* SceneTree::find_scene(SceneId scene) const noexcept {
    for (const SceneRecord* record : scenes_) {
        if (record->id == scene) {
            return record;
        }
    }
    return nullptr;
}

Status SceneTree::unload_all() noexcept {
    // Collected first: `unload` edits the records, and a loop over a container it is editing is the
    // shape of bug this milestone is meant to be too early for.
    Array<SceneId> loaded(allocator());
    for (const SceneRecord* record : scenes_) {
        if (record->status != SceneStatus::Unloaded) {
            if (Status pushed = loaded.push_back(record->id); !pushed) {
                return pushed;
            }
        }
    }
    for (const SceneId id : loaded) {
        if (Status unloaded = unload(id); !unloaded) {
            return unloaded;
        }
    }
    return ok();
}

Expected<SceneId, Error> SceneTree::begin_load(const SceneDescription& description,
                                               LoadMode mode) noexcept {
    if (!initialized_) {
        return fail(ErrorCode::Unavailable, "the scene tree has not been initialized");
    }
    if (mode == LoadMode::Replace) {
        if (Status cleared = unload_all(); !cleared) {
            return make_unexpected(cleared.error());
        }
    }

    void* block = allocator().allocate(sizeof(SceneRecord), alignof(SceneRecord));
    if (block == nullptr) {
        return fail(ErrorCode::OutOfMemory, "could not allocate a scene record");
    }
    auto* record = ::new (block) SceneRecord(allocator());
    record->id = next_scene_++;
    record->name = description.name;
    record->status = SceneStatus::Loading;
    record->pending = description.nodes;
    record->total = static_cast<u32>(description.nodes.size());

    // The scene root is the single node a load adds under the tree root, so unloading it takes the
    // whole scene with it and an additive load cannot leave orphans behind.
    Expected<Entity, Error> root = instantiate(description.name, root_, Name(), record->id);
    if (!root) {
        record->~SceneRecord();
        allocator().deallocate(block, sizeof(SceneRecord), alignof(SceneRecord));
        return make_unexpected(root.error());
    }
    record->root = *root;

    if (Status pushed = scenes_.push_back(record); !pushed) {
        record->~SceneRecord();
        allocator().deallocate(block, sizeof(SceneRecord), alignof(SceneRecord));
        return make_unexpected(pushed.error());
    }
    if (record->total == 0) {
        record->status = SceneStatus::Active;
    }
    return record->id;
}

Expected<SceneId, Error> SceneTree::load(const SceneDescription& description,
                                         LoadMode mode) noexcept {
    Expected<SceneId, Error> scene = begin_load(description, mode);
    if (!scene) {
        return scene;
    }
    SceneRecord* record = find_scene(*scene);
    if (record == nullptr) {
        return fail(ErrorCode::Internal, "the scene that was just begun cannot be found");
    }
    u32 created = 0;
    if (Status advanced = advance_load(*record, record->total, created); !advanced) {
        return make_unexpected(advanced.error());
    }
    return *scene;
}

Status SceneTree::apply_node_desc(const NodeDesc& desc, Entity entity) noexcept {
    Node node(*this, entity);
    if (Status placed = node.set_local_transform(desc.local_transform); !placed) {
        return placed;
    }
    if (!desc.visible) {
        if (Status hidden = node.set_visible(false); !hidden) {
            return hidden;
        }
    }
    if (!desc.enabled) {
        if (Status disabled = node.set_enabled(false); !disabled) {
            return disabled;
        }
    }
    if (!desc.alias.is_empty()) {
        if (Status aliased = set_alias(node, desc.alias); !aliased) {
            return aliased;
        }
    }
    if (desc.behaviour.is_empty()) {
        return ok();
    }
    const BehaviourTypeId type = behaviours_.find(desc.behaviour);
    if (type == kInvalidBehaviour) {
        return fail(ErrorCode::NotFound, "this scene names a behaviour that is not registered");
    }
    return behaviours_.attach(*this, node, type);
}

Status SceneTree::instantiate_scene_node(SceneRecord& record, const NodeDesc& desc) noexcept {
    // A description is topologically ordered by construction — `NodeDesc::parent` names an earlier
    // index — so one forward pass resolves every parent without a second walk.
    Entity parent = record.root;
    if (desc.parent != NodeDesc::kNoParent) {
        if (desc.parent >= record.created.size()) {
            return fail(ErrorCode::InvalidArgument,
                        "a scene node names a parent that does not precede it");
        }
        parent = record.created[desc.parent];
    }
    Expected<Entity, Error> entity = instantiate(desc.name, parent, desc.node_template, record.id);
    if (!entity) {
        return make_unexpected(entity.error());
    }
    if (Status pushed = record.created.push_back(*entity); !pushed) {
        return pushed;
    }
    return apply_node_desc(desc, *entity);
}

Status SceneTree::advance_load(SceneRecord& record, u32 budget, u32& created) noexcept {
    created = 0;
    while (record.next < record.total && created < budget) {
        if (Status made = instantiate_scene_node(record, record.pending[record.next]); !made) {
            return made;
        }
        ++record.next;
        ++created;
    }
    if (record.next == record.total) {
        // "The scene SHALL become active only once fully instantiated." The index of created
        // entities is released here: from now on membership is the `SceneRef` component, which is
        // what an unload reads.
        record.status = SceneStatus::Active;
        record.pending = Span<const NodeDesc>();
        record.created.clear();
        if (Status shrunk = record.created.shrink_to_fit(); !shrunk) {
            return shrunk;
        }
    }
    return ok();
}

Expected<u32, Error> SceneTree::pump_loads(u32 budget) noexcept {
    u32 remaining = budget;
    u32 total = 0;
    // Oldest first, so a scene requested earlier finishes earlier and a stream of load requests
    // cannot starve the first one.
    for (SceneRecord* record : scenes_) {
        if (remaining == 0) {
            break;
        }
        if (record->status != SceneStatus::Loading) {
            continue;
        }
        u32 created = 0;
        if (Status advanced = advance_load(*record, remaining, created); !advanced) {
            return make_unexpected(advanced.error());
        }
        remaining -= created;
        total += created;
    }
    return total;
}

Status SceneTree::collect_scene_entities(SceneId scene, Array<Entity>& out) noexcept {
    ecs::QueryDesc desc(allocator());
    if (Status read = desc.read(ids_.scene_ref); !read) {
        return read;
    }
    ecs::Query query(*world_, std::move(desc));
    const ComponentTypeId scene_ref = ids_.scene_ref;
    Status collected = ok();
    Status iterated =
        query.for_each_chunk([&out, &collected, scene, scene_ref](ecs::QueryChunk& chunk) noexcept {
            if (!collected) {
                return;
            }
            const Span<const SceneRef> refs = chunk.read<SceneRef>(scene_ref);
            const Span<const Entity> entities = chunk.entities();
            for (usize row = 0; row < entities.size(); ++row) {
                if (refs[row].scene != scene) {
                    continue;
                }
                if (Status pushed = out.push_back(entities[row]); !pushed) {
                    collected = pushed;
                    return;
                }
            }
        });
    if (!iterated) {
        return iterated;
    }
    if (!collected) {
        return collected;
    }
    std::ranges::sort(
        out, [](Entity left, Entity right) noexcept { return left.index() < right.index(); });
    return ok();
}

Status SceneTree::unload(SceneId scene) noexcept {
    SceneRecord* record = find_scene(scene);
    if (record == nullptr || record->status == SceneStatus::Unloaded) {
        return fail(ErrorCode::NotFound, "no scene with this id is loaded");
    }
    Array<Entity> members(allocator());
    if (Status collected = collect_scene_entities(scene, members); !collected) {
        return collected;
    }
    // The root goes first and takes its subtree with it; the sweep afterwards catches anything a
    // game reparented out of the scene, which is what makes "exactly those entities" true rather
    // than "the ones still under the root".
    if (world_->is_alive(record->root)) {
        if (Status destroyed = destroy_node(Node(*this, record->root)); !destroyed) {
            return destroyed;
        }
    }
    for (const Entity entity : members) {
        if (!world_->is_alive(entity)) {
            continue;
        }
        if (Status destroyed = destroy_node(Node(*this, entity)); !destroyed) {
            return destroyed;
        }
    }
    record->status = SceneStatus::Unloaded;
    record->pending = Span<const NodeDesc>();
    record->created.clear();
    record->root = ecs::kNoEntity;
    return ok();
}

Expected<SceneInfo, Error> SceneTree::scene(SceneId scene) const noexcept {
    const SceneRecord* record = find_scene(scene);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "no scene with this id has been loaded");
    }
    SceneInfo info;
    info.id = record->id;
    info.name = record->name;
    info.status = record->status;
    info.instantiated = record->next;
    info.total = record->total;
    info.root = record->root;
    return info;
}

Node SceneTree::scene_root(SceneId scene) noexcept {
    const SceneRecord* record = find_scene(scene);
    return (record == nullptr) ? Node() : Node(*this, record->root);
}

Status SceneTree::scenes(Array<SceneInfo>& out) const noexcept {
    for (const SceneRecord* record : scenes_) {
        if (record->status == SceneStatus::Unloaded) {
            continue;
        }
        Expected<SceneInfo, Error> info = scene(record->id);
        if (!info) {
            return make_unexpected(info.error());
        }
        if (Status pushed = out.push_back(*info); !pushed) {
            return pushed;
        }
    }
    return ok();
}

}  // namespace cy::scene
