// `Node` — transforms, interpolation, visibility and enablement. Tasks 3.1.3 and 3.1.5.
//
// The authored value is what a node writes and the derived value is what propagation writes, and
// this file never crosses that line: `set_world_transform` derives a *local* transform and writes
// that, leaving `WorldTransform` to the one system responsible for it. A node that wrote both would
// be the shadow copy design.md §3 exists to forbid, one function later.

#include <cy/scene/node.h>

#include <cy/scene/propagation.h>
#include <cy/scene/tree.h>

namespace cy::scene {

Transform Node::local_transform() const noexcept {
    if (!valid()) {
        return Transform::identity();
    }
    const auto* held =
        tree_->world().get<LocalTransform>(entity_, tree_->components().local_transform);
    return (held == nullptr) ? Transform::identity() : held->value;
}

Status Node::set_local_transform(const Transform& value) const noexcept {
    if (!valid()) {
        return fail(ErrorCode::NotFound, "set_local_transform() on a node that is not valid");
    }
    auto* held =
        tree_->world().get_mut<LocalTransform>(entity_, tree_->components().local_transform);
    if (held == nullptr) {
        return fail(ErrorCode::NotFound, "this node has no LocalTransform component");
    }
    held->value = value;
    return mark_transform_changed();
}

Transform Node::world_transform() const noexcept {
    if (!valid()) {
        return Transform::identity();
    }
    const auto* held =
        tree_->world().get<WorldTransform>(entity_, tree_->components().world_transform);
    return (held == nullptr) ? Transform::identity() : held->value;
}

Status Node::set_world_transform(const Transform& value) const noexcept {
    if (!valid()) {
        return fail(ErrorCode::NotFound, "set_world_transform() on a node that is not valid");
    }
    const Node holder = parent();
    // `scene-graph-and-nodes`: "the corresponding `LocalTransform` SHALL be derived from the
    // parent's inverse and written, keeping the local value authoritative". For a root the parent
    // frame is the identity, so the two are the same value — which is the specification's "a root
    // node's `LocalTransform` SHALL equal its `WorldTransform`" read from the other end.
    const Transform local = holder.valid() ? (inverse(holder.world_transform()) * value) : value;
    return set_local_transform(local);
}

Status Node::mark_transform_changed() const noexcept {
    if (!valid()) {
        return fail(ErrorCode::NotFound, "mark_transform_changed() on a node that is not valid");
    }
    return scene::mark_transform_changed(*tree_, entity_);
}

bool Node::interpolated() const noexcept {
    return valid() && tree_->world().has(entity_, tree_->components().interpolated_transform);
}

Status Node::set_interpolated(bool interpolated) const noexcept {
    if (!valid()) {
        return fail(ErrorCode::NotFound, "set_interpolated() on a node that is not valid");
    }
    const ComponentTypeId component = tree_->components().interpolated_transform;
    if (interpolated == tree_->world().has(entity_, component)) {
        return ok();
    }
    if (!interpolated) {
        return tree_->world().remove(entity_, component);
    }
    // Seeded with the current world transform rather than the identity: a node that opts in
    // mid-frame must not blend from the origin on the frame it opted in.
    //
    // Both fields are `Presentation`, so they are constructed rather than assigned — a classified
    // field has no converting constructor, which is what stops a value being laundered into one by
    // copying it. See components.h.
    const InterpolatedTransform initial{determinism::Presentation<Transform>(world_transform()),
                                        determinism::Presentation<bool>(false)};
    return tree_->world().add(entity_, component, &initial);
}

Status Node::teleport() const noexcept {
    if (!valid()) {
        return fail(ErrorCode::NotFound, "teleport() on a node that is not valid");
    }
    auto* held = tree_->world().get_mut<InterpolatedTransform>(
        entity_, tree_->components().interpolated_transform);
    if (held == nullptr) {
        // Not interpolated, so there is nothing to suppress. Reporting success is the honest
        // answer: the caller asked for "do not blend this node", and it will not be blended.
        return ok();
    }
    // An authoritative caller writing a presentation field: legal, and the direction the firewall
    // leaves open. Gameplay saying "do not blend this node" is authority flowing downhill; what it
    // could not do is read the blend back.
    held->teleport.write(determinism::AuthoritativeContext{}, true);
    return ok();
}

Transform Node::render_transform(f32 alpha) const noexcept {
    const Transform current = world_transform();
    if (!valid()) {
        return current;
    }
    const auto* held = tree_->world().get<InterpolatedTransform>(
        entity_, tree_->components().interpolated_transform);
    // The witness this function carries is what it is: `render_transform` is presentation, it is
    // named after the thing it is for, and a presentation reader may read anything. An
    // authoritative caller cannot obtain this value at all — there is no overload that yields it.
    constexpr determinism::PresentationContext kPresentation;
    if (held == nullptr || held->teleport.read(kPresentation)) {
        return current;
    }
    return interpolate(held->previous.read(kPresentation), current, alpha);
}

bool Node::visible() const noexcept {
    if (!valid()) {
        return false;
    }
    const auto* held = tree_->world().get<NodeFlags>(entity_, tree_->components().flags);
    return (held == nullptr) || held->visible;
}

bool Node::enabled() const noexcept {
    if (!valid()) {
        return false;
    }
    const auto* held = tree_->world().get<NodeFlags>(entity_, tree_->components().flags);
    return (held == nullptr) || held->enabled;
}

Status Node::set_visible(bool visible) const noexcept {
    if (!valid()) {
        return fail(ErrorCode::NotFound, "set_visible() on a node that is not valid");
    }
    auto* held = tree_->world().get_mut<NodeFlags>(entity_, tree_->components().flags);
    if (held == nullptr) {
        return fail(ErrorCode::NotFound, "this node has no NodeFlags component");
    }
    held->visible = visible;
    return mark_flags_changed(*tree_, entity_);
}

Status Node::set_enabled(bool enabled) const noexcept {
    if (!valid()) {
        return fail(ErrorCode::NotFound, "set_enabled() on a node that is not valid");
    }
    auto* held = tree_->world().get_mut<NodeFlags>(entity_, tree_->components().flags);
    if (held == nullptr) {
        return fail(ErrorCode::NotFound, "this node has no NodeFlags component");
    }
    held->enabled = enabled;
    return mark_flags_changed(*tree_, entity_);
}

bool Node::effective_visible() const noexcept {
    // The tag *is* the effective state, not a copy of it (components.h). Absence is visible, so a
    // node that has never been propagated reads as visible rather than as unknown.
    return valid() && !tree_->world().has(entity_, tree_->components().hidden);
}

bool Node::effective_enabled() const noexcept {
    return valid() && !tree_->world().has(entity_, tree_->components().disabled);
}

}  // namespace cy::scene
