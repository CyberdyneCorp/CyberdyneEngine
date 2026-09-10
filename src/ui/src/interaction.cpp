// Routing, focus and the layer stack. M8.b task 9.2.

#include <cy/ui/interaction.h>

#include <cmath>

namespace cy::ui {
namespace {

[[nodiscard]] bool is_navigation(UiAction action) noexcept {
    return action == UiAction::NavigateUp || action == UiAction::NavigateDown ||
           action == UiAction::NavigateLeft || action == UiAction::NavigateRight;
}

[[nodiscard]] Vec2 centre_of(const Rect& rect) noexcept {
    return Vec2{rect.x + (rect.width * 0.5F), rect.y + (rect.height * 0.5F)};
}

/// Whether `candidate` lies in `direction` from `origin`, and how good a candidate it is. Lower is
/// better; a negative score means "not in that direction at all".
[[nodiscard]] f32 directional_score(Vec2 origin, Vec2 candidate, UiAction direction) noexcept {
    const Vec2 delta{candidate.x - origin.x, candidate.y - origin.y};
    f32 along = 0.0F;
    f32 across = 0.0F;
    switch (direction) {
        case UiAction::NavigateLeft:
            along = -delta.x;
            across = std::fabs(delta.y);
            break;
        case UiAction::NavigateRight:
            along = delta.x;
            across = std::fabs(delta.y);
            break;
        case UiAction::NavigateUp:
            along = -delta.y;
            across = std::fabs(delta.x);
            break;
        case UiAction::NavigateDown:
            along = delta.y;
            across = std::fabs(delta.x);
            break;
        default:
            return -1.0F;
    }
    if (along <= 0.0F) {
        return -1.0F;
    }
    // Distance along the axis, plus a penalty for drifting across it. Twice the weight across, so a
    // control directly below wins over one diagonally below and slightly nearer — which is what a
    // player pressing "down" means.
    return along + (across * 2.0F);
}

}  // namespace

const char* ui_action_name(UiAction action) noexcept {
    switch (action) {
        case UiAction::Accept:
            return "UI.Accept";
        case UiAction::Cancel:
            return "UI.Cancel";
        case UiAction::NavigateUp:
            return "UI.NavigateUp";
        case UiAction::NavigateDown:
            return "UI.NavigateDown";
        case UiAction::NavigateLeft:
            return "UI.NavigateLeft";
        case UiAction::NavigateRight:
            return "UI.NavigateRight";
        case UiAction::NextTab:
            return "UI.NextTab";
        case UiAction::PreviousTab:
            return "UI.PreviousTab";
        case UiAction::Context:
            return "UI.Context";
        case UiAction::ScrollUp:
            return "UI.ScrollUp";
        case UiAction::ScrollDown:
            return "UI.ScrollDown";
        case UiAction::Count:
            break;
    }
    return "UI.Unknown";
}

const char* layer_kind_name(LayerKind kind) noexcept {
    switch (kind) {
        case LayerKind::Game:
            return "game";
        case LayerKind::Hud:
            return "hud";
        case LayerKind::Overlay:
            return "overlay";
        case LayerKind::Modal:
            return "modal";
        case LayerKind::System:
            return "system";
        case LayerKind::Count:
            break;
    }
    return "unknown";
}

LayerBehaviour default_behaviour(LayerKind kind) noexcept {
    LayerBehaviour behaviour;
    switch (kind) {
        // A GAME LAYER AND A HUD ARE THE SAME DECLARATION, and they are two enumerators because
        // they are two things: a HUD is drawn by the interface and the game layer is what the
        // interface sits over. Neither takes focus, blocks input or answers back navigation.
        case LayerKind::Game:
        case LayerKind::Hud:
            // A heads-up display is DRAWN and not INTERACTIVE: it does not take focus and does not
            // stop anything reaching the game beneath it.
            behaviour.captures_focus = false;
            behaviour.blocks_input = false;
            behaviour.handles_back = false;
            break;
        case LayerKind::Overlay:
            // "WHEN an inventory Overlay is open THEN the HUD SHALL remain visible but
            // non-interactive, per the overlay's declared behaviour."
            behaviour.captures_focus = true;
            behaviour.blocks_input = true;
            behaviour.handles_back = true;
            behaviour.dim_beneath = 0.35F;
            behaviour.enter_seconds = 0.15F;
            behaviour.exit_seconds = 0.12F;
            break;
        case LayerKind::Modal:
            behaviour.captures_focus = true;
            behaviour.blocks_input = true;
            behaviour.handles_back = true;
            behaviour.dim_beneath = 0.6F;
            behaviour.enter_seconds = 0.18F;
            behaviour.exit_seconds = 0.12F;
            break;
        case LayerKind::System:
            // A system layer — a platform overlay, a fatal error — blocks everything and does not
            // answer to back navigation.
            behaviour.captures_focus = true;
            behaviour.blocks_input = true;
            behaviour.handles_back = false;
            break;
        case LayerKind::Count:
            break;
    }
    return behaviour;
}

Interaction::Interaction(Allocator& allocator) noexcept
    : layers_(allocator),
      neighbours_(allocator),
      captures_(allocator),
      entered_(allocator),
      exited_(allocator),
      hovered_(allocator) {}

Status Interaction::push_layer(const Layer& layer) noexcept {
    Layer pushed = layer;
    pushed.restore_focus = focus_;
    pushed.elapsed = 0.0F;
    if (Status added = layers_.push_back(pushed); !added) {
        return added;
    }
    if (pushed.behaviour.captures_focus) {
        // Focus moves INTO the layer. Which element takes it is the caller's, through `set_focus`;
        // what this does is drop the focus that belonged to the layer beneath, so a keystroke
        // cannot reach it.
        focus_ = kNoElement;
    }
    return ok();
}

Status Interaction::pop_layer() noexcept {
    if (layers_.empty()) {
        return make_unexpected(Error{ErrorCode::NotFound, "the layer stack is empty", 0});
    }
    const Layer top = layers_[layers_.size() - 1];
    layers_.pop_back();
    if (top.behaviour.captures_focus) {
        // "WHEN a modal is dismissed THEN focus SHALL return to the element that had it before the
        // modal opened."
        focus_ = top.restore_focus;
    }
    return ok();
}

const Layer* Interaction::top_layer() const noexcept {
    return layers_.empty() ? nullptr : &layers_[layers_.size() - 1];
}

bool Interaction::within_layer(const ElementStore& store, ElementId element,
                               ElementId root) noexcept {
    if (!root.is_valid()) {
        return true;
    }
    for (ElementId walk = element; walk.is_valid();) {
        if (walk == root) {
            return true;
        }
        const Hierarchy* node = store.hierarchy(walk);
        if (node == nullptr) {
            break;
        }
        walk = node->parent;
    }
    return false;
}

Status Interaction::set_focus(const ElementStore& store, ElementId element) noexcept {
    if (!element.is_valid()) {
        focus_ = kNoElement;
        return ok();
    }
    if (!store.alive(element)) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such element", 0});
    }
    const ElementFlags flags = store.flags(element);
    if (!has_flag(flags, ElementFlags::Focusable) || has_flag(flags, ElementFlags::Disabled)) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "this element does not take focus", 0});
    }
    const Layer* top = top_layer();
    if (top != nullptr && top->behaviour.captures_focus &&
        !within_layer(store, element, top->root)) {
        // FOCUS IS SCOPED TO THE ACTIVE LAYER, and this refusal is the scope. "WHEN directional
        // navigation reaches the edge of a modal THEN focus SHALL NOT escape to elements beneath
        // it."
        return make_unexpected(
            Error{ErrorCode::PermissionDenied, "that element is outside the focused layer", 0});
    }
    focus_ = element;
    return ok();
}

Status Interaction::set_neighbour(ElementId from, UiAction direction, ElementId to) noexcept {
    if (!is_navigation(direction)) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "a neighbour is declared for a direction", 0});
    }
    for (Neighbour& entry : neighbours_.span()) {
        if (entry.from == from && entry.direction == direction) {
            entry.to = to;
            return ok();
        }
    }
    return neighbours_.push_back(Neighbour{from, direction, to});
}

ElementId Interaction::navigate(const ElementStore& store, UiAction direction) noexcept {
    if (!is_navigation(direction)) {
        return focus_;
    }
    // AN EXPLICIT NEIGHBOUR WINS. "focus SHALL move to the explicitly declared right neighbour, or
    // to the nearest focusable element in that direction within the active layer."
    for (const Neighbour& entry : neighbours_.span()) {
        if (entry.from == focus_ && entry.direction == direction && store.alive(entry.to)) {
            if (set_focus(store, entry.to)) {
                return focus_;
            }
        }
    }

    const LayoutOutput* current = store.layout_output(focus_);
    if (current == nullptr) {
        return focus_;
    }
    const Vec2 origin = centre_of(current->rect);
    const Layer* top = top_layer();
    const ElementId scope = (top != nullptr) ? top->root : kNoElement;

    ElementId best;
    f32 best_score = -1.0F;
    // A WALK OF THE TREE, not a spatial index. A UI with tens of thousands of elements has a few
    // dozen focusable ones in the active layer, and an index maintained per frame to answer a query
    // made twice a second would cost more than it saves.
    for (const ElementId root : store.roots()) {
        Array<ElementId> stack(store.allocator());
        if (!stack.push_back(root)) {
            break;
        }
        while (!stack.empty()) {
            const ElementId element = stack[stack.size() - 1];
            stack.pop_back();
            const Hierarchy* node = store.hierarchy(element);
            if (node == nullptr) {
                continue;
            }
            for (ElementId child = node->first_child; child.is_valid();) {
                const Hierarchy* child_node = store.hierarchy(child);
                if (child_node == nullptr) {
                    break;
                }
                if (!stack.push_back(child)) {
                    break;
                }
                child = child_node->next_sibling;
            }
            if (element == focus_) {
                continue;
            }
            const ElementFlags flags = store.flags(element);
            if (!has_flag(flags, ElementFlags::Focusable) ||
                has_flag(flags, ElementFlags::Disabled) ||
                !has_flag(flags, ElementFlags::Visible)) {
                continue;
            }
            if (!within_layer(store, element, scope)) {
                continue;
            }
            const LayoutOutput* output = store.layout_output(element);
            if (output == nullptr) {
                continue;
            }
            const f32 score = directional_score(origin, centre_of(output->rect), direction);
            if (score < 0.0F) {
                continue;
            }
            if (best_score < 0.0F || score < best_score) {
                best_score = score;
                best = element;
            }
        }
    }
    if (best.is_valid() && set_focus(store, best)) {
        return focus_;
    }
    return focus_;
}

ElementId Interaction::hit_test(const ElementStore& store, ElementId root, Vec2 position,
                                u32& bubbled) const noexcept {
    const LayoutOutput* output = store.layout_output(root);
    const ElementFlags flags = store.flags(root);
    if (output == nullptr || !has_flag(flags, ElementFlags::Visible) ||
        has_flag(flags, ElementFlags::Collapsed)) {
        return kNoElement;
    }
    // THE CLIP IS PART OF THE TEST. An element scrolled out of its container is not hit, which is
    // the same rect the paint pass culls against — one answer, not two.
    if (!output->clip.empty() && !output->clip.contains(position)) {
        return kNoElement;
    }

    const Hierarchy* node = store.hierarchy(root);
    if (node != nullptr) {
        // Children in reverse order: the last child is drawn on top, so it is hit first.
        for (ElementId child = node->last_child; child.is_valid();) {
            const Hierarchy* child_node = store.hierarchy(child);
            if (child_node == nullptr) {
                break;
            }
            const ElementId previous = child_node->previous_sibling;
            const ElementId hit = hit_test(store, child, position, bubbled);
            if (hit.is_valid()) {
                return hit;
            }
            child = previous;
        }
    }
    if (!output->rect.contains(position)) {
        return kNoElement;
    }
    switch (store.hit_test_mode(root)) {
        case HitTestMode::Block:
            return root;
        case HitTestMode::Pass:
            // Handled, and the walk continues beneath — the caller sees the element and keeps
            // looking, which is what `Pass` means.
            ++bubbled;
            return root;
        case HitTestMode::Ignore:
            return kNoElement;
    }
    return kNoElement;
}

RouteResult Interaction::probe(const ElementStore& store, Vec2 position) const noexcept {
    RouteResult result;
    for (usize index = layers_.size(); index > 0; --index) {
        const Layer& layer = layers_[index - 1];
        u32 bubbled = 0;
        const ElementId hit =
            layer.root.is_valid() ? hit_test(store, layer.root, position, bubbled) : kNoElement;
        if (hit.is_valid()) {
            result.target = hit;
            result.layer = static_cast<u32>(index - 1);
            result.bubbled = bubbled;
            return result;
        }
        if (layer.behaviour.blocks_input) {
            // THE LAYER STACK DECIDES CONSUMPTION. A modal that was missed still stops the event:
            // "WHEN a modal layer is open THEN it SHALL block input from reaching gameplay, by the
            // layer's declared behaviour rather than by game code checking a flag."
            result.blocked_by_layer = true;
            result.layer = static_cast<u32>(index - 1);
            return result;
        }
    }
    return result;
}

ElementId Interaction::capture(u32 pointer) const noexcept {
    for (const Capture& entry : captures_.span()) {
        if (entry.pointer == pointer) {
            return entry.element;
        }
    }
    return kNoElement;
}

Status Interaction::set_capture(u32 pointer, ElementId element) noexcept {
    for (Capture& entry : captures_.span()) {
        if (entry.pointer == pointer) {
            entry.element = element;
            return ok();
        }
    }
    return captures_.push_back(Capture{pointer, element});
}

Expected<RouteResult, Error> Interaction::route_pointer(ElementStore& store,
                                                        const PointerEvent& event) noexcept {
    RouteResult result;
    const ElementId captured = capture(event.pointer);
    if (captured.is_valid() && store.alive(captured)) {
        // CAPTURE WINS OVER THE HIT TEST. "WHEN a slider is pressed and the pointer moves outside
        // it THEN the slider SHALL continue receiving move events until release."
        result.target = captured;
        result.captured = true;
    } else {
        result = probe(store, event.position);
    }

    // Hover: what entered and what left, so a caller can raise enter and exit events and a style
    // can read `:hover`.
    const ElementId hovered = result.target;
    for (usize index = 0; index < hovered_.size();) {
        const ElementId previous = hovered_[index];
        if (previous == hovered) {
            ++index;
            continue;
        }
        if (store.alive(previous)) {
            const Status cleared = store.set_flags(
                previous,
                without_flag(store.flags(previous), ElementFlags::Hovered | ElementFlags::Pressed));
            (void)cleared;
            if (Status pushed = exited_.push_back(previous); !pushed) {
                return make_unexpected(pushed.error());
            }
        }
        hovered_.remove_unordered(index);
    }
    if (hovered.is_valid()) {
        bool already = false;
        for (const ElementId element : hovered_.span()) {
            if (element == hovered) {
                already = true;
                break;
            }
        }
        if (!already) {
            if (Status pushed = hovered_.push_back(hovered); !pushed) {
                return make_unexpected(pushed.error());
            }
            if (Status pushed = entered_.push_back(hovered); !pushed) {
                return make_unexpected(pushed.error());
            }
        }
        ElementFlags flags = store.flags(hovered) | ElementFlags::Hovered;
        if (event.pressed) {
            flags = flags | ElementFlags::Pressed;
        }
        if (event.released) {
            flags = without_flag(flags, ElementFlags::Pressed);
        }
        if (Status set = store.set_flags(hovered, flags); !set) {
            return make_unexpected(set.error());
        }
        if (event.pressed) {
            if (Status set = set_capture(event.pointer, hovered); !set) {
                return make_unexpected(set.error());
            }
            if (has_flag(flags, ElementFlags::Focusable) &&
                !has_flag(flags, ElementFlags::Disabled)) {
                const Status focused = set_focus(store, hovered);
                (void)focused;  // A press outside the focused layer leaves focus where it was.
            }
        }
    }
    if (event.released) {
        if (captured.is_valid() && store.alive(captured)) {
            const Status cleared = store.set_flags(
                captured, without_flag(store.flags(captured), ElementFlags::Pressed));
            (void)cleared;
        }
        if (Status set = set_capture(event.pointer, kNoElement); !set) {
            return make_unexpected(set.error());
        }
    }
    return result;
}

Expected<RouteResult, Error> Interaction::route_action(ElementStore& store,
                                                       UiAction action) noexcept {
    RouteResult result;
    if (is_navigation(action)) {
        result.target = navigate(store, action);
        result.layer =
            layers_.empty() ? RouteResult::kNoLayer : static_cast<u32>(layers_.size() - 1);
        return result;
    }
    if (action == UiAction::Cancel) {
        // BACK NAVIGATION goes to the topmost layer that declares a handler, and pops it.
        for (usize index = layers_.size(); index > 0; --index) {
            if (!layers_[index - 1].behaviour.handles_back) {
                if (layers_[index - 1].behaviour.blocks_input) {
                    result.blocked_by_layer = true;
                    result.layer = static_cast<u32>(index - 1);
                    return result;
                }
                continue;
            }
            result.layer = static_cast<u32>(index - 1);
            result.target = layers_[index - 1].root;
            if (Status popped = pop_layer(); !popped) {
                return make_unexpected(popped.error());
            }
            return result;
        }
        return result;
    }

    // Everything else goes to focus, and bubbles to its ancestors until something blocks.
    ElementId walk = focus_;
    while (walk.is_valid()) {
        const ElementFlags flags = store.flags(walk);
        if (!has_flag(flags, ElementFlags::Disabled) &&
            store.hit_test_mode(walk) != HitTestMode::Ignore) {
            result.target = walk;
            result.layer =
                layers_.empty() ? RouteResult::kNoLayer : static_cast<u32>(layers_.size() - 1);
            return result;
        }
        const Hierarchy* node = store.hierarchy(walk);
        if (node == nullptr) {
            break;
        }
        ++result.bubbled;
        walk = node->parent;
    }
    return result;
}

void Interaction::clear_events() noexcept {
    entered_.clear();
    exited_.clear();
}

}  // namespace cy::ui
