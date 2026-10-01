// SPDX-License-Identifier: MIT
// Script behaviours on scene nodes, and `node_find`. ABI 1.5, `add-swift-m12-gaps`. See
// include/cy/game_backend/scene_bridge.h, and tests/test_scene_backend.cpp for the proof.

#include <cy/game_backend/scene_bridge.h>

#include <cy/abi/errors.h>
#include <cy/core/memory/ownership.h>
#include <cy/core/values/name.h>
#include <cy/scene/tree.h>

#include <cstring>
#include <string_view>

namespace cy::game_backend {

ScriptSceneBridge::ScriptSceneBridge(Allocator& allocator, scene::SceneTree& tree, abi::Host& host,
                                     abi::BehaviourRuntime& runtime) noexcept
    : allocator_(allocator),
      tree_(&tree),
      host_(host),
      runtime_(runtime),
      types_(allocator),
      attached_(allocator) {}

ScriptSceneBridge::~ScriptSceneBridge() {
    for (TypeBinding* binding : types_) {
        binding->~TypeBinding();
        allocator_.deallocate(static_cast<void*>(binding), sizeof(TypeBinding),
                              alignof(TypeBinding));
    }
}

bool ScriptSceneBridge::owns_type(const char* name) const noexcept {
    for (const TypeBinding* binding : types_) {
        if (std::strcmp(binding->name, name) == 0) {
            return true;
        }
    }
    return false;
}

Status ScriptSceneBridge::add_type(const char* name) noexcept {
    Expected<UniquePtr<TypeBinding>, Error> made = make_unique<TypeBinding>(allocator_);
    if (!made) {
        return make_unexpected(made.error());
    }
    if (Status reserved = types_.reserve(types_.size() + 1U); !reserved) {
        return reserved;
    }
    made.value()->bridge = this;
    made.value()->name = name;

    scene::BehaviourDesc desc;
    desc.name = name;
    desc.user = made.value().get();
    desc.on_create = &ScriptSceneBridge::on_create;
    desc.on_destroy = &ScriptSceneBridge::on_destroy;
    desc.on_enter_tree = &ScriptSceneBridge::on_enter_tree;
    desc.on_ready = &ScriptSceneBridge::on_ready;
    desc.on_enable = &ScriptSceneBridge::on_enable;
    desc.on_disable = &ScriptSceneBridge::on_disable;
    desc.on_exit_tree = &ScriptSceneBridge::on_exit_tree;
    // A script, so per-instance if it ever had a tick here — which it does not: the runtime keeps
    // `onFixedUpdate` and `onUpdate`, and a type with no per-tick callback is in no dispatch list.
    desc.invokes_script = true;
    const Expected<scene::BehaviourTypeId, Error> added =
        tree_->behaviours().add(tree_->world(), desc);
    if (!added) {
        return make_unexpected(added.error());
    }
    (void)types_.push_back(made.value().release());
    return ok();
}

Expected<u32, Error> ScriptSceneBridge::sync_types() noexcept {
    u32 added = 0;
    for (const abi::BehaviourRecord* record : host_.behaviours) {
        if (record->generation != host_.generation || owns_type(record->name)) {
            continue;
        }
        if (tree_->behaviours().find(Name::intern(record->name)) != scene::kInvalidBehaviour) {
            return fail(ErrorCode::AlreadyExists,
                        "a native scene behaviour already has a script behaviour's name");
        }
        if (Status registered = add_type(record->name); !registered) {
            return make_unexpected(registered.error());
        }
        ++added;
    }
    return added;
}

Status ScriptSceneBridge::attach(scene::Node node, const char* name) noexcept {
    if (name == nullptr || !owns_type(name)) {
        return fail(ErrorCode::NotFound, "no script behaviour of that name; call sync_types()");
    }
    const scene::BehaviourTypeId type = tree_->behaviours().find(Name::intern(name));
    create_failed_ = false;
    if (Status attached = tree_->behaviours().attach(*tree_, node, type); !attached) {
        return attached;
    }
    if (create_failed_) {
        // The scene attached a behaviour whose script the module refused to create. Detaching
        // leaves the node as it was; `onDestroy` finds no slot and does nothing.
        (void)tree_->behaviours().detach(*tree_, node);
        return fail(ErrorCode::Unavailable, "the module refused to create the script behaviour");
    }
    return ok();
}

u32 ScriptSceneBridge::slot_of(CyEntity entity) const noexcept {
    for (const Attached& entry : attached_) {
        if (entry.entity == entity) {
            return entry.slot;
        }
    }
    return scene::kNoBehaviourInstance;
}

void ScriptSceneBridge::created(CyEntity entity, const char* name) noexcept {
    const Expected<u32, Error> slot = runtime_.create(name, entity);
    if (!slot || !attached_.push_back(Attached{entity, *slot})) {
        if (slot) {
            (void)runtime_.destroy(*slot);
        }
        create_failed_ = true;
    }
}

void ScriptSceneBridge::destroyed(CyEntity entity) noexcept {
    for (usize index = 0; index < attached_.size(); ++index) {
        if (attached_[index].entity != entity) {
            continue;
        }
        (void)runtime_.destroy(attached_[index].slot);
        attached_[index] = attached_[attached_.size() - 1U];
        (void)attached_.resize(attached_.size() - 1U);  // shrinking never allocates
        return;
    }
}

void ScriptSceneBridge::on_create(const scene::BehaviourContext& context) noexcept {
    const auto* binding = static_cast<const TypeBinding*>(context.user);
    binding->bridge->created(abi::to_abi(context.node.entity()), binding->name);
}

void ScriptSceneBridge::on_destroy(const scene::BehaviourContext& context) noexcept {
    const auto* binding = static_cast<const TypeBinding*>(context.user);
    binding->bridge->destroyed(abi::to_abi(context.node.entity()));
}

void ScriptSceneBridge::forward(const scene::BehaviourContext& context,
                                abi::TreeCallback callback) noexcept {
    const auto* binding = static_cast<const TypeBinding*>(context.user);
    const u32 slot = binding->bridge->slot_of(abi::to_abi(context.node.entity()));
    if (slot != scene::kNoBehaviourInstance) {
        (void)binding->bridge->runtime_.tree_callback(slot, callback);
    }
}

void ScriptSceneBridge::on_enter_tree(const scene::BehaviourContext& context) noexcept {
    forward(context, abi::TreeCallback::EnterTree);
}

void ScriptSceneBridge::on_ready(const scene::BehaviourContext& context) noexcept {
    forward(context, abi::TreeCallback::Ready);
}

void ScriptSceneBridge::on_enable(const scene::BehaviourContext& context) noexcept {
    forward(context, abi::TreeCallback::Enable);
}

void ScriptSceneBridge::on_disable(const scene::BehaviourContext& context) noexcept {
    forward(context, abi::TreeCallback::Disable);
}

void ScriptSceneBridge::on_exit_tree(const scene::BehaviourContext& context) noexcept {
    forward(context, abi::TreeCallback::ExitTree);
}

CyResult ScriptSceneBridge::find(CyEntity from, const char* path, CyEntity& out) const noexcept {
    const std::string_view text(path);
    scene::Node found;
    if (text.front() == '/') {
        found = tree_->find(text);
    } else {
        const scene::Node origin = tree_->node(abi::from_abi(from));
        found = origin.valid() ? origin.find(text) : scene::Node();
    }
    if (!found.valid()) {
        return abi::report(CY_RESULT_NOT_FOUND, "no node at that path");
    }
    out = abi::to_abi(found.entity());
    return CY_RESULT_OK;
}

void bind_scene(cy::abi::Host& host, ScriptSceneBridge* bridge) noexcept {
    host.game.scene = bridge;
}

}  // namespace cy::game_backend
