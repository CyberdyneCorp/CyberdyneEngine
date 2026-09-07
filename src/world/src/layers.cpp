#include <cy/world/layers.h>

namespace cy::world {

const char* layer_kind_name(LayerKind kind) noexcept {
    switch (kind) {
        case LayerKind::EditorOnly:
            return "EditorOnly";
        case LayerKind::Runtime:
            return "Runtime";
        case LayerKind::Scenario:
            return "Scenario";
        case LayerKind::Variant:
            return "Variant";
        case LayerKind::System:
            return "System";
    }
    return "unknown";
}

const char* layer_state_name(LayerState state) noexcept {
    switch (state) {
        case LayerState::Unloaded:
            return "Unloaded";
        case LayerState::Loaded:
            return "Loaded";
        case LayerState::Activated:
            return "Activated";
    }
    return "unknown";
}

LayerTable::LayerTable(Allocator& allocator) noexcept : layers_(allocator), changes_(allocator) {}

Status LayerTable::declare(LayerId id, LayerKind kind, const char* name) noexcept {
    if (!id.is_valid()) {
        return fail(ErrorCode::InvalidArgument, "a layer identifier is never zero");
    }
    if (Layer* existing = mutable_find(id); existing != nullptr) {
        // A re-declaration updates the definition and leaves the STATE alone: a cook that reloads
        // its layer definitions while a scenario is running must not reset the scenario.
        existing->kind = kind;
        existing->name = name;
        return ok();
    }
    Layer layer;
    layer.id = id;
    layer.kind = kind;
    layer.name = name;
    // Runtime and system layers are activated by default; a scenario or a variant is not, because
    // the whole point of one is that something switches it on.
    layer.state = (kind == LayerKind::Runtime || kind == LayerKind::System) ? LayerState::Activated
                                                                            : LayerState::Unloaded;
    return layers_.push_back(layer);
}

const Layer* LayerTable::find(LayerId id) const noexcept {
    for (const Layer& layer : layers_.span()) {
        if (layer.id == id) {
            return &layer;
        }
    }
    return nullptr;
}

Layer* LayerTable::mutable_find(LayerId id) noexcept {
    for (Layer& layer : layers_.span()) {
        if (layer.id == id) {
            return &layer;
        }
    }
    return nullptr;
}

Status LayerTable::set_state(LayerId id, LayerState state) noexcept {
    Layer* layer = mutable_find(id);
    if (layer == nullptr) {
        return fail(ErrorCode::NotFound, "no layer with that identifier is declared");
    }
    if (layer->kind == LayerKind::EditorOnly && state != LayerState::Unloaded) {
        return fail(ErrorCode::InvalidArgument,
                    "an editor-only layer is not cooked into runtime data and cannot be activated");
    }
    if (layer->state == state) {
        return ok();
    }
    layer->state = state;
    return changes_.push_back(StateChange{id, state});
}

LayerState LayerTable::state_of(LayerId id) const noexcept {
    if (id == kDefaultLayer) {
        // The default layer is always activated, and is not required to be declared: a world with
        // no use for layers should not have to declare one to have its entities exist.
        const Layer* declared = find(id);
        return (declared == nullptr) ? LayerState::Activated : declared->state;
    }
    const Layer* layer = find(id);
    return (layer == nullptr) ? LayerState::Unloaded : layer->state;
}

bool LayerTable::is_cooked(LayerId id) const noexcept {
    const Layer* layer = find(id);
    return layer == nullptr ? id == kDefaultLayer : layer->kind != LayerKind::EditorOnly;
}

Status LayerTable::drain_changes(Array<StateChange>& out) noexcept {
    if (Status appended = out.append(changes_.span()); !appended) {
        return appended;
    }
    changes_.clear();
    return ok();
}

}  // namespace cy::world
