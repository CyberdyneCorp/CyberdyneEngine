#include <cy/scene/serialization/document.h>

#include <utility>

namespace cy::scene::serialization {
namespace {

/// The index at which `id` sits or would sit in a list held in ascending id order.
template <class T, class Key>
[[nodiscard]] usize lower_bound_by_id(const Array<T>& items, Key id) noexcept {
    usize low = 0;
    usize high = items.size();
    while (low < high) {
        const usize middle = low + ((high - low) / 2);
        if (items[middle].id < id) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

/// Move the item at the end of `items` back to `index`, keeping the order. `Array` has no insert-at
/// and adding one would be a container change for a control-plane caller; an authoring document is
/// built once and read many times.
template <class T>
void rotate_into_place(Array<T>& items, usize index) noexcept {
    for (usize position = items.size() - 1; position > index; --position) {
        T moved = std::move(items[position - 1]);
        items[position - 1] = std::move(items[position]);
        items[position] = std::move(moved);
    }
}

}  // namespace

const char* asset_kind_name(AssetKind kind) noexcept {
    switch (kind) {
        case AssetKind::Prefab:
            return "prefab";
        case AssetKind::Scene:
            return "scene";
        case AssetKind::World:
            return "world";
    }
    return "<invalid>";
}

const char* cook_mode_name(CookMode mode) noexcept {
    switch (mode) {
        case CookMode::Embedded:
            return "embedded";
        case CookMode::Packed:
            return "packed";
    }
    return "<invalid>";
}

const char* flatten_policy_name(FlattenPolicy policy) noexcept {
    switch (policy) {
        case FlattenPolicy::Automatic:
            return "auto";
        case FlattenPolicy::Keep:
            return "keep";
        case FlattenPolicy::Flatten:
            return "flatten";
    }
    return "<invalid>";
}

// --- DocumentEntity
// -------------------------------------------------------------------------------

ComponentData* DocumentEntity::find(reflect::TypeId type) noexcept {
    for (ComponentData& component : components_) {
        if (component.type == type) {
            return &component;
        }
    }
    return nullptr;
}

const ComponentData* DocumentEntity::find(reflect::TypeId type) const noexcept {
    for (const ComponentData& component : components_) {
        if (component.type == type) {
            return &component;
        }
    }
    return nullptr;
}

Expected<ComponentData*, Error> DocumentEntity::ensure(reflect::TypeId type) noexcept {
    if (!type.valid()) {
        return fail(ErrorCode::InvalidArgument, "a component addresses its type by TypeId");
    }
    if (ComponentData* existing = find(type); existing != nullptr) {
        return existing;
    }
    // Components are kept in TypeId order so that two documents describing the same entity produce
    // the same file: the text form's determinism is a property of every list in it being ordered.
    usize index = 0;
    while (index < components_.size() && components_[index].type < type) {
        ++index;
    }
    ComponentData fresh{type, serialize::ValueRecord(components_.allocator())};
    fresh.record.set_type(type);
    if (Status added = components_.push_back(std::move(fresh)); !added) {
        return make_unexpected(added.error());
    }
    rotate_into_place(components_, index);
    return &components_[index];
}

bool DocumentEntity::remove(reflect::TypeId type) noexcept {
    for (usize index = 0; index < components_.size(); ++index) {
        if (components_[index].type == type) {
            components_.erase(index);
            return true;
        }
    }
    return false;
}

// --- Instance
// -------------------------------------------------------------------------------------

LocalId mapped_local(Span<const InstanceMapping> mapping, LocalId source) noexcept {
    usize low = 0;
    usize high = mapping.size();
    while (low < high) {
        const usize middle = low + ((high - low) / 2);
        if (mapping[middle].source < source) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low < mapping.size() && mapping[low].source == source) {
        return mapping[low].local;
    }
    return kNoLocalId;
}

Status add_mapping(Array<InstanceMapping>& mapping, LocalId source, LocalId local) noexcept {
    if (!source.valid() || !local.valid()) {
        return fail(ErrorCode::InvalidArgument, "a mapping entry names two valid local ids");
    }
    usize index = 0;
    while (index < mapping.size() && mapping[index].source < source) {
        ++index;
    }
    if (index < mapping.size() && mapping[index].source == source) {
        return fail(ErrorCode::AlreadyExists, "the source id is already mapped");
    }
    if (Status added = mapping.push_back(InstanceMapping{source, local}); !added) {
        return added;
    }
    rotate_into_place(mapping, index);
    return ok();
}

LocalId Instance::local_of(LocalId source_id) const noexcept {
    return mapped_local(mapping_.span(), source_id);
}

// --- Document
// -------------------------------------------------------------------------------------

Expected<TextRef, Error> Document::intern(std::string_view value) noexcept {
    if (value.empty()) {
        return TextRef{};
    }
    if (value.size() > 0xFFFF'FFFFULL) {
        return fail(ErrorCode::OutOfRange, "a name longer than four gigabytes is not a name");
    }
    const TextRef ref{static_cast<u32>(text_.size()), static_cast<u32>(value.size())};
    if (Status appended = text_.append(Span<const char>(value.data(), value.size())); !appended) {
        return make_unexpected(appended.error());
    }
    return ref;
}

std::string_view Document::text(TextRef ref) const noexcept {
    if (ref.length == 0 || static_cast<usize>(ref.offset) + ref.length > text_.size()) {
        return {};
    }
    return {text_.data() + ref.offset, ref.length};
}

Expected<DocumentEntity*, Error> Document::add_entity(LocalId parent,
                                                      std::string_view name) noexcept {
    return add_entity_with_id(allocate_id(), parent, name);
}

Expected<DocumentEntity*, Error> Document::add_entity_with_id(LocalId entity_id, LocalId parent,
                                                              std::string_view name) noexcept {
    if (!entity_id.valid()) {
        return fail(ErrorCode::InvalidArgument, "zero is 'no entity' and is never an entity's id");
    }
    const usize index = lower_bound_by_id(entities_, entity_id);
    if (index < entities_.size() && entities_[index].id == entity_id) {
        return fail(ErrorCode::AlreadyExists,
                    "an entity with that local id is already in the document");
    }
    if (find_instance(entity_id) != nullptr) {
        return fail(ErrorCode::AlreadyExists,
                    "an instance already holds that local id; the two share one id space");
    }

    const Expected<TextRef, Error> interned = intern(name);
    if (!interned) {
        return make_unexpected(interned.error());
    }

    DocumentEntity entity(allocator());
    entity.id = entity_id;
    entity.parent = parent;
    entity.name = interned.value();
    if (Status added = entities_.push_back(std::move(entity)); !added) {
        return make_unexpected(added.error());
    }
    rotate_into_place(entities_, index);
    if (entity_id.value() >= next_local_id_) {
        next_local_id_ = entity_id.value() + 1;
    }
    return &entities_[index];
}

DocumentEntity* Document::find_entity(LocalId entity_id) noexcept {
    const usize index = lower_bound_by_id(entities_, entity_id);
    return (index < entities_.size() && entities_[index].id == entity_id) ? &entities_[index]
                                                                          : nullptr;
}

const DocumentEntity* Document::find_entity(LocalId entity_id) const noexcept {
    const usize index = lower_bound_by_id(entities_, entity_id);
    return (index < entities_.size() && entities_[index].id == entity_id) ? &entities_[index]
                                                                          : nullptr;
}

Expected<Instance*, Error> Document::add_instance(AssetId source, LocalId parent,
                                                  std::string_view name) noexcept {
    return add_instance_with_id(allocate_id(), source, parent, name);
}

Expected<Instance*, Error> Document::add_instance_with_id(LocalId instance_id, AssetId source,
                                                          LocalId parent,
                                                          std::string_view name) noexcept {
    if (!instance_id.valid()) {
        return fail(ErrorCode::InvalidArgument,
                    "zero is 'no entity' and is never an instance's id");
    }
    if (source.is_nil()) {
        return fail(ErrorCode::InvalidArgument, "an instance names the document it places");
    }
    const usize index = lower_bound_by_id(instances_, instance_id);
    if (index < instances_.size() && instances_[index].id == instance_id) {
        return fail(ErrorCode::AlreadyExists, "an instance with that local id is already here");
    }
    if (find_entity(instance_id) != nullptr) {
        return fail(ErrorCode::AlreadyExists,
                    "an entity already holds that local id; the two share one id space");
    }

    const Expected<TextRef, Error> interned = intern(name);
    if (!interned) {
        return make_unexpected(interned.error());
    }

    Instance instance(allocator());
    instance.id = instance_id;
    instance.parent = parent;
    instance.name = interned.value();
    instance.source = source;
    if (Status added = instances_.push_back(std::move(instance)); !added) {
        return make_unexpected(added.error());
    }
    rotate_into_place(instances_, index);
    if (instance_id.value() >= next_local_id_) {
        next_local_id_ = instance_id.value() + 1;
    }
    return &instances_[index];
}

Instance* Document::find_instance(LocalId instance_id) noexcept {
    const usize index = lower_bound_by_id(instances_, instance_id);
    return (index < instances_.size() && instances_[index].id == instance_id) ? &instances_[index]
                                                                              : nullptr;
}

const Instance* Document::find_instance(LocalId instance_id) const noexcept {
    const usize index = lower_bound_by_id(instances_, instance_id);
    return (index < instances_.size() && instances_[index].id == instance_id) ? &instances_[index]
                                                                              : nullptr;
}

Expected<ExposedParameter*, Error> Document::add_parameter(std::string_view name,
                                                           serialize::WireType wire) noexcept {
    return add_parameter_with_id(ParameterId(next_parameter_id_), name, wire);
}

Expected<ExposedParameter*, Error> Document::add_parameter_with_id(
    ParameterId parameter_id, std::string_view name, serialize::WireType wire) noexcept {
    if (!parameter_id.valid()) {
        return fail(ErrorCode::InvalidArgument, "zero is not a parameter identifier");
    }
    const usize index = lower_bound_by_id(parameters_, parameter_id);
    if (index < parameters_.size() && parameters_[index].id == parameter_id) {
        return fail(ErrorCode::AlreadyExists, "a parameter with that identifier already exists");
    }
    if (find_parameter(name) != nullptr) {
        return fail(ErrorCode::AlreadyExists, "a parameter with that name already exists");
    }

    const Expected<TextRef, Error> interned = intern(name);
    if (!interned) {
        return make_unexpected(interned.error());
    }

    ExposedParameter parameter(allocator());
    parameter.id = parameter_id;
    parameter.name = interned.value();
    parameter.wire = wire;
    if (Status added = parameters_.push_back(std::move(parameter)); !added) {
        return make_unexpected(added.error());
    }
    rotate_into_place(parameters_, index);
    if (parameter_id.value() >= next_parameter_id_) {
        next_parameter_id_ = parameter_id.value() + 1;
    }
    return &parameters_[index];
}

ExposedParameter* Document::find_parameter(ParameterId parameter_id) noexcept {
    const usize index = lower_bound_by_id(parameters_, parameter_id);
    return (index < parameters_.size() && parameters_[index].id == parameter_id)
               ? &parameters_[index]
               : nullptr;
}

const ExposedParameter* Document::find_parameter(ParameterId parameter_id) const noexcept {
    const usize index = lower_bound_by_id(parameters_, parameter_id);
    return (index < parameters_.size() && parameters_[index].id == parameter_id)
               ? &parameters_[index]
               : nullptr;
}

const ExposedParameter* Document::find_parameter(std::string_view name) const noexcept {
    for (const ExposedParameter& parameter : parameters_) {
        if (text(parameter.name) == name) {
            return &parameter;
        }
    }
    return nullptr;
}

}  // namespace cy::scene::serialization
