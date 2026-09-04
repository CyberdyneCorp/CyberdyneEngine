#include <cy/scene/serialization/resolve.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace cy::scene::serialization {
namespace {

/// A source local id and the id it became in the container.
struct IdPair {
    LocalId source;
    LocalId local;
};

/// The working id map for one placement. Small and linear: a placement contributes the entities of
/// one prefab, which is tens, not millions.
class IdMap {
public:
    explicit IdMap(Allocator& allocator) noexcept : pairs_(allocator) {}

    [[nodiscard]] Status add(LocalId source, LocalId local) noexcept {
        return pairs_.push_back(IdPair{source, local});
    }

    [[nodiscard]] LocalId find(LocalId source) const noexcept {
        for (const IdPair& pair : pairs_) {
            if (pair.source == source) {
                return pair.local;
            }
        }
        return kNoLocalId;
    }

private:
    Array<IdPair> pairs_;
};

}  // namespace

Expected<ResolvedComponent*, Error> ensure_resolved_component(ResolvedEntity& entity,
                                                              reflect::TypeId type,
                                                              Allocator& allocator) noexcept {
    for (ResolvedComponent& component : entity.components) {
        if (component.type == type) {
            return &component;
        }
    }
    ResolvedComponent fresh{type, serialize::ValueRecord(allocator),
                            serialize::ValueRecord(allocator), Array<FieldProvenance>(allocator)};
    fresh.record.set_type(type);
    fresh.inherited.set_type(type);
    if (Status added = entity.components.push_back(std::move(fresh)); !added) {
        return make_unexpected(added.error());
    }
    return &entity.components.back();
}

ResolvedComponent* find_resolved_component(ResolvedEntity& entity, reflect::TypeId type) noexcept {
    for (ResolvedComponent& component : entity.components) {
        if (component.type == type) {
            return &component;
        }
    }
    return nullptr;
}

const ResolvedComponent* find_resolved_component(const ResolvedEntity& entity,
                                                 reflect::TypeId type) noexcept {
    for (const ResolvedComponent& component : entity.components) {
        if (component.type == type) {
            return &component;
        }
    }
    return nullptr;
}

namespace {

/// Record where a field's value came from, keeping the value the layer below had.
///
/// Called before the new value is written, so `component.record` still holds the inherited one.
/// That ordering is the whole implementation of "report ... what the inherited value is": there is
/// no second resolve with the top layer removed, because the value was captured on the way past.
[[nodiscard]] Status note_provenance(ResolvedComponent& component, reflect::FieldId field,
                                     ValueSource source, AssetId asset) noexcept {
    const serialize::FieldValue* previous = component.record.find(field);
    if (previous != nullptr && !component.inherited.contains(field)) {
        const Span<const u8> bytes = component.record.bytes(*previous);
        if (Status kept = component.inherited.set(field, previous->wire, bytes.data(),
                                                  static_cast<u32>(bytes.size()));
            !kept) {
            return kept;
        }
    }

    const Provenance provenance{source, asset, previous != nullptr};
    for (FieldProvenance& entry : component.provenance) {
        if (entry.field == field) {
            entry.provenance = provenance;
            return ok();
        }
    }
    return component.provenance.push_back(FieldProvenance{field, provenance});
}

/// Rewrite every `LocalRef` in one record from the source id space to the container's.
///
/// This is the authoring-level ancestor of the cooked reference fixup, and it exists for the same
/// reason: a reference is a number whose meaning is relative to the document it was written in, so
/// moving the data between id spaces has to move the references with it. A reference the map does
/// not know is left alone rather than nulled, because the only references that reach here and are
/// not in the map are ones that already point outside the placement.
[[nodiscard]] Status remap_references(serialize::ValueRecord& record, const IdMap& map) noexcept {
    for (usize index = 0; index < record.size(); ++index) {
        const serialize::FieldValue value = record.fields()[index];
        if (value.wire != serialize::WireType::LocalRef) {
            continue;
        }
        const Expected<u32, Error> local = record.local_reference(value.id);
        if (!local) {
            return make_unexpected(local.error());
        }
        const LocalId mapped = map.find(LocalId(local.value()));
        if (!mapped.valid()) {
            continue;
        }
        if (Status written = record.set_local_reference(value.id, mapped.value()); !written) {
            return written;
        }
    }
    return ok();
}

}  // namespace

Expected<cy::Transform, Error> read_transform_of(const ResolvedEntity& entity,
                                                 const TransformBinding& binding) noexcept {
    const ResolvedComponent* component = find_resolved_component(entity, binding.component);
    if (component == nullptr) {
        return cy::Transform::identity();
    }
    const Span<const u8> bytes = component->record.bytes(binding.field);
    if (bytes.empty()) {
        return cy::Transform::identity();
    }
    if (bytes.size() != sizeof(cy::Transform)) {
        return fail(ErrorCode::InvalidArgument,
                    "the bound transform field is not the width of a cy::Transform");
    }
    cy::Transform transform;
    std::memcpy(&transform, bytes.data(), sizeof(transform));
    return transform;
}

Status write_transform_of(ResolvedEntity& entity, const TransformBinding& binding,
                          const cy::Transform& transform, AssetId asset, ValueSource source,
                          Allocator& allocator) noexcept {
    const Expected<ResolvedComponent*, Error> component =
        ensure_resolved_component(entity, binding.component, allocator);
    if (!component) {
        return make_unexpected(component.error());
    }
    if (Status noted = note_provenance(**component, binding.field, source, asset); !noted) {
        return noted;
    }
    return (*component)
        ->record.set(binding.field, serialize::WireType::Bytes, &transform,
                     static_cast<u32>(sizeof(transform)));
}

const char* difference_kind_name(GraphDifference::Kind kind) noexcept {
    switch (kind) {
        case GraphDifference::Kind::EntityAdded:
            return "entity_added";
        case GraphDifference::Kind::EntityRemoved:
            return "entity_removed";
        case GraphDifference::Kind::ComponentAdded:
            return "component_added";
        case GraphDifference::Kind::ComponentRemoved:
            return "component_removed";
        case GraphDifference::Kind::FieldChanged:
            return "field_changed";
        case GraphDifference::Kind::ParentChanged:
            return "parent_changed";
    }
    return "<invalid>";
}

// --- ResolvedGraph
// --------------------------------------------------------------------------------

ResolvedEntity* ResolvedGraph::find(LocalId id) noexcept {
    for (ResolvedEntity& entity : entities_) {
        if (entity.id == id) {
            return &entity;
        }
    }
    return nullptr;
}

const ResolvedEntity* ResolvedGraph::find(LocalId id) const noexcept {
    for (const ResolvedEntity& entity : entities_) {
        if (entity.id == id) {
            return &entity;
        }
    }
    return nullptr;
}

ResolvedParameter* ResolvedGraph::find_parameter(ParameterId id) noexcept {
    for (ResolvedParameter& parameter : parameters_) {
        if (parameter.id == id) {
            return &parameter;
        }
    }
    return nullptr;
}

Expected<TextRef, Error> ResolvedGraph::intern(std::string_view value) noexcept {
    if (value.empty()) {
        return TextRef{};
    }
    const TextRef ref{static_cast<u32>(text_.size()), static_cast<u32>(value.size())};
    if (Status appended = text_.append(Span<const char>(value.data(), value.size())); !appended) {
        return make_unexpected(appended.error());
    }
    return ref;
}

std::string_view ResolvedGraph::text(TextRef ref) const noexcept {
    if (ref.length == 0 || static_cast<usize>(ref.offset) + ref.length > text_.size()) {
        return {};
    }
    return {text_.data() + ref.offset, ref.length};
}

Expected<ResolvedEntity*, Error> ResolvedGraph::add(LocalId id) noexcept {
    if (!id.valid()) {
        return fail(ErrorCode::InvalidArgument, "zero is 'no entity' and is never an entity's id");
    }
    if (find(id) != nullptr) {
        return fail(ErrorCode::AlreadyExists, "the resolved graph already holds that local id");
    }
    // Ascending id order. Every list this module writes to a file or walks deterministically is
    // ordered, and this one feeds both the text form and the cook.
    usize index = 0;
    while (index < entities_.size() && entities_[index].id < id) {
        ++index;
    }
    ResolvedEntity entity;
    entity.id = id;
    entity.components = Array<ResolvedComponent>(allocator());
    if (Status added = entities_.push_back(std::move(entity)); !added) {
        return make_unexpected(added.error());
    }
    for (usize position = entities_.size() - 1; position > index; --position) {
        ResolvedEntity moved = std::move(entities_[position - 1]);
        entities_[position - 1] = std::move(entities_[position]);
        entities_[position] = std::move(moved);
    }
    return &entities_[index];
}

const Provenance* ResolvedGraph::provenance_of(LocalId entity, reflect::TypeId component,
                                               reflect::FieldId field) const noexcept {
    const ResolvedEntity* found = find(entity);
    if (found == nullptr) {
        return nullptr;
    }
    const ResolvedComponent* data = find_resolved_component(*found, component);
    if (data == nullptr) {
        return nullptr;
    }
    for (const FieldProvenance& entry : data->provenance) {
        if (entry.field == field) {
            return &entry.provenance;
        }
    }
    return nullptr;
}

Span<const u8> ResolvedGraph::inherited_value(LocalId entity, reflect::TypeId component,
                                              reflect::FieldId field) const noexcept {
    const ResolvedEntity* found = find(entity);
    if (found == nullptr) {
        return {};
    }
    const ResolvedComponent* data = find_resolved_component(*found, component);
    if (data == nullptr) {
        return {};
    }
    return data->inherited.bytes(field);
}

namespace {

/// One pass of the resolver over one document. Holds the state that would otherwise be six
/// parameters threaded through every helper.
class Resolver {
public:
    Resolver(const Library& library, const ResolveOptions& options, ResolveReport& report) noexcept
        : library_(&library), options_(&options), report_(&report) {}

    /// Resolve `id` into `out`. `depth` is the variant nesting, reported rather than limited.
    [[nodiscard]] Status run(AssetId id, ResolvedGraph& out, u32 depth) noexcept;

private:
    [[nodiscard]] Status expand_base(Document& document, ResolvedGraph& out, u32& next_auto,
                                     u32 depth) noexcept;
    [[nodiscard]] Status expand_instance(Document& document, Instance& instance, ResolvedGraph& out,
                                         u32& next_auto, u32 depth) noexcept;
    [[nodiscard]] static Status copy_own_entities(const Document& document,
                                                  ResolvedGraph& out) noexcept;
    [[nodiscard]] static Status copy_own_parameters(const Document& document,
                                                    ResolvedGraph& out) noexcept;

    [[nodiscard]] Status splice(const ResolvedGraph& sub, Span<const InstanceMapping> mapping,
                                LocalId attach_parent, const cy::Transform& placement,
                                bool keep_together, ResolvedGraph& out, u32& next_auto,
                                IdMap& map) noexcept;

    [[nodiscard]] static Status take_parameters(ResolvedGraph& sub, const IdMap& map,
                                                ResolvedGraph& out) noexcept;
    [[nodiscard]] Status apply_arguments(Span<const ParameterArgument> arguments,
                                         ResolvedGraph& out) noexcept;

    [[nodiscard]] Status apply_overrides(OverrideList& overrides, const IdMap& map,
                                         ValueSource source, AssetId owner, LocalId instance,
                                         ResolvedGraph& out) noexcept;
    [[nodiscard]] Status apply_one(Override& item, const IdMap& map, ValueSource source,
                                   AssetId owner, ResolvedGraph& out,
                                   ConflictKind& conflict) noexcept;
    [[nodiscard]] Status set_field(Override& item, ResolvedEntity& entity, ValueSource source,
                                   AssetId owner, ConflictKind& conflict) noexcept;
    [[nodiscard]] static Status remove_subtree(ResolvedGraph& out, LocalId root) noexcept;

    /// True when the type is registered and does not carry the field. Unknown types answer false:
    /// a component from a disabled module is not evidence that its field was deleted.
    [[nodiscard]] bool field_was_removed(reflect::TypeId type,
                                         reflect::FieldId field) const noexcept;

    [[nodiscard]] Status record_conflict(AssetId asset, LocalId instance, usize index,
                                         ConflictKind kind, const OverrideTarget& target) noexcept;

    const Library* library_;
    const ResolveOptions* options_;
    ResolveReport* report_;
};

Status Resolver::record_conflict(AssetId asset, LocalId instance, usize index, ConflictKind kind,
                                 const OverrideTarget& target) noexcept {
    return report_->conflicts.push_back(ConflictReport{asset, instance, index, kind, target});
}

bool Resolver::field_was_removed(reflect::TypeId type, reflect::FieldId field) const noexcept {
    if (options_->types == nullptr) {
        return false;
    }
    const reflect::FieldIndex* fields = options_->types->fields(type);
    if (fields == nullptr) {
        return false;
    }
    return fields->find(field) == nullptr;
}

Status Resolver::splice(const ResolvedGraph& sub, Span<const InstanceMapping> mapping,
                        LocalId attach_parent, const cy::Transform& placement, bool keep_together,
                        ResolvedGraph& out, u32& next_auto, IdMap& map) noexcept {
    for (const ResolvedEntity& entity : sub.entities()) {
        LocalId local = mapped_local(mapping, entity.id);
        if (!local.valid()) {
            local = LocalId(next_auto++);
        }
        if (Status added = map.add(entity.id, local); !added) {
            return added;
        }
    }

    const bool place = placement != cy::Transform::identity();
    if (place && !options_->transform.valid()) {
        return fail(ErrorCode::InvalidArgument,
                    "a placement carries a transform but no transform binding was supplied");
    }

    for (const ResolvedEntity& entity : sub.entities()) {
        const LocalId local = map.find(entity.id);
        const Expected<ResolvedEntity*, Error> created = out.add(local);
        if (!created) {
            return make_unexpected(created.error());
        }
        ResolvedEntity& target = **created;

        const Expected<TextRef, Error> name = out.intern(sub.text(entity.name));
        if (!name) {
            return make_unexpected(name.error());
        }
        target.name = name.value();
        target.motion = entity.motion;
        target.flatten = entity.flatten;
        target.origin = entity.origin.is_nil() ? sub.source : entity.origin;
        target.origin_local = entity.origin_local.valid() ? entity.origin_local : entity.id;

        const bool is_root = !entity.parent.valid();
        target.parent = is_root ? attach_parent : map.find(entity.parent);
        // A packed scene instance is "retained as a runtime unit with its own local space", so its
        // hierarchy is not the cook's to flatten however static its contents look.
        if (keep_together && target.flatten == FlattenPolicy::Automatic) {
            target.flatten = FlattenPolicy::Keep;
        }

        for (const ResolvedComponent& component : entity.components) {
            const Expected<ResolvedComponent*, Error> copy =
                ensure_resolved_component(target, component.type, out.allocator());
            if (!copy) {
                return make_unexpected(copy.error());
            }
            if (Status cloned = component.record.clone_into((*copy)->record); !cloned) {
                return cloned;
            }
            if (Status cloned = component.inherited.clone_into((*copy)->inherited); !cloned) {
                return cloned;
            }
            for (const FieldProvenance& entry : component.provenance) {
                if (Status kept = (*copy)->provenance.push_back(entry); !kept) {
                    return kept;
                }
            }
            if (Status remapped = remap_references((*copy)->record, map); !remapped) {
                return remapped;
            }
        }

        if (place && is_root) {
            const Expected<cy::Transform, Error> local_transform =
                read_transform_of(target, options_->transform);
            if (!local_transform) {
                return make_unexpected(local_transform.error());
            }
            if (Status written = write_transform_of(target, options_->transform,
                                                    placement * local_transform.value(), sub.source,
                                                    ValueSource::Instance, out.allocator());
                !written) {
                return written;
            }
        }
    }
    return ok();
}

Status Resolver::take_parameters(ResolvedGraph& sub, const IdMap& map,
                                 ResolvedGraph& out) noexcept {
    for (ResolvedParameter& parameter : sub.parameters()) {
        ResolvedParameter copy;
        copy.id = parameter.id;
        copy.wire = parameter.wire;
        copy.has_value = parameter.has_value;
        copy.value = Array<u8>(out.allocator());
        copy.bindings = Array<ParameterBinding>(out.allocator());
        if (Status kept = copy.value.append(parameter.value.span()); !kept) {
            return kept;
        }
        const Expected<TextRef, Error> name = out.intern(sub.text(parameter.name));
        if (!name) {
            return make_unexpected(name.error());
        }
        copy.name = name.value();
        for (const ParameterBinding& binding : parameter.bindings) {
            const LocalId entity = map.find(binding.entity);
            if (!entity.valid()) {
                continue;  // Bound to an entity this placement did not contribute; nothing to
                           // write.
            }
            if (Status kept = copy.bindings.push_back(
                    ParameterBinding{entity, binding.component, binding.field});
                !kept) {
                return kept;
            }
        }
        if (Status added = out.parameters().push_back(std::move(copy)); !added) {
            return added;
        }
    }
    return ok();
}

Status Resolver::apply_arguments(Span<const ParameterArgument> arguments,
                                 ResolvedGraph& out) noexcept {
    for (const ParameterArgument& argument : arguments) {
        ResolvedParameter* parameter = out.find_parameter(argument.id);
        if (parameter == nullptr) {
            // A parameter the source no longer exposes. The specification's answer for a lost
            // override target is a conflict rather than a silent drop, and an argument is an
            // override of a parameter, so it gets the same treatment.
            OverrideTarget target;
            if (Status recorded = record_conflict(out.source, kNoLocalId, argument.id.value(),
                                                  ConflictKind::MissingField, target);
                !recorded) {
                return recorded;
            }
            continue;
        }
        parameter->value.clear();
        if (Status written = parameter->value.append(argument.value.span()); !written) {
            return written;
        }
        parameter->wire = argument.wire;
        parameter->has_value = true;
    }
    return ok();
}

/// Write every parameter in `[first, last)` into the fields it is bound to.
///
/// One copy of this loop, called from the two places parameters are written — when a placement
/// supplies them, and when a resolve finishes with some still exposed. Two copies would be two
/// answers to what provenance a parameter write records.
[[nodiscard]] Status write_bindings(ResolvedGraph& out, usize first, usize last, AssetId asset,
                                    ResolveReport& report) noexcept {
    for (usize index = first; index < last && index < out.parameters().size(); ++index) {
        const ResolvedParameter& parameter = out.parameters()[index];
        if (!parameter.has_value) {
            continue;
        }
        for (const ParameterBinding& binding : parameter.bindings) {
            ResolvedEntity* entity = out.find(binding.entity);
            if (entity == nullptr) {
                continue;
            }
            const Expected<ResolvedComponent*, Error> component =
                ensure_resolved_component(*entity, binding.component, out.allocator());
            if (!component) {
                return make_unexpected(component.error());
            }
            if (Status noted =
                    note_provenance(**component, binding.field, ValueSource::Parameter, asset);
                !noted) {
                return noted;
            }
            if (Status written =
                    (*component)
                        ->record.set(binding.field, parameter.wire, parameter.value.data(),
                                     static_cast<u32>(parameter.value.size()));
                !written) {
                return written;
            }
            ++report.parameters_applied;
        }
    }
    return ok();
}

Status Resolver::set_field(Override& item, ResolvedEntity& entity, ValueSource source,
                           AssetId owner, ConflictKind& conflict) noexcept {
    ResolvedComponent* component = find_resolved_component(entity, item.target().component);
    if (component == nullptr) {
        conflict = ConflictKind::MissingComponent;
        return ok();
    }
    if (field_was_removed(item.target().component, item.target().field)) {
        conflict = ConflictKind::MissingField;
        return ok();
    }
    const Span<const u8> value = item.payload().bytes(item.target().field);
    const serialize::FieldValue* descriptor = item.payload().find(item.target().field);
    if (descriptor == nullptr) {
        return fail(ErrorCode::InvalidArgument,
                    "a set_field override carries the value under the field it targets");
    }
    if (Status noted = note_provenance(*component, item.target().field, source, owner); !noted) {
        return noted;
    }
    return component->record.set(item.target().field, descriptor->wire, value.data(),
                                 static_cast<u32>(value.size()));
}

Status Resolver::remove_subtree(ResolvedGraph& out, LocalId root) noexcept {
    Array<LocalId> doomed(out.allocator());
    if (Status seeded = doomed.push_back(root); !seeded) {
        return seeded;
    }
    for (usize index = 0; index < doomed.size(); ++index) {
        const LocalId parent = doomed[index];
        for (const ResolvedEntity& entity : out.entities()) {
            if (entity.parent == parent) {
                if (Status added = doomed.push_back(entity.id); !added) {
                    return added;
                }
            }
        }
    }
    for (const LocalId id : doomed) {
        for (usize index = 0; index < out.entities().size(); ++index) {
            if (out.entities()[index].id == id) {
                out.entities().erase(index);
                break;
            }
        }
    }
    return ok();
}

Status Resolver::apply_one(Override& item, const IdMap& map, ValueSource source, AssetId owner,
                           ResolvedGraph& out, ConflictKind& conflict) noexcept {
    LocalId entity_id = map.find(item.target().entity);
    if (!entity_id.valid()) {
        // An `add_entity` names an entity the source has never heard of, because the container
        // invented it. Everything else naming an unmapped id is addressing something that is gone.
        if (item.op() != OverrideOp::AddEntity) {
            conflict = ConflictKind::MissingEntity;
            return ok();
        }
        entity_id = item.target().entity;
    }

    switch (item.op()) {
        case OverrideOp::AddEntity: {
            const LocalId parent = item.parent().valid() ? map.find(item.parent()) : kNoLocalId;
            if (item.parent().valid() && !parent.valid()) {
                conflict = ConflictKind::MissingParent;
                return ok();
            }
            const Expected<ResolvedEntity*, Error> added = out.add(entity_id);
            if (!added) {
                return make_unexpected(added.error());
            }
            (*added)->parent = parent;
            (*added)->origin = owner;
            (*added)->origin_local = item.target().entity;
            return ok();
        }
        case OverrideOp::RemoveEntity: {
            if (out.find(entity_id) == nullptr) {
                conflict = ConflictKind::MissingEntity;
                return ok();
            }
            return remove_subtree(out, entity_id);
        }
        default:
            break;
    }

    ResolvedEntity* entity = out.find(entity_id);
    if (entity == nullptr) {
        conflict = ConflictKind::MissingEntity;
        return ok();
    }

    switch (item.op()) {
        case OverrideOp::SetField:
            return set_field(item, *entity, source, owner, conflict);
        case OverrideOp::AddComponent: {
            const Expected<ResolvedComponent*, Error> component =
                ensure_resolved_component(*entity, item.target().component, out.allocator());
            if (!component) {
                return make_unexpected(component.error());
            }
            return (*component)->record.overlay(item.payload());
        }
        case OverrideOp::RemoveComponent: {
            for (usize index = 0; index < entity->components.size(); ++index) {
                if (entity->components[index].type == item.target().component) {
                    entity->components.erase(index);
                    return ok();
                }
            }
            conflict = ConflictKind::MissingComponent;
            return ok();
        }
        case OverrideOp::ReparentEntity: {
            const LocalId parent = item.parent().valid() ? map.find(item.parent()) : kNoLocalId;
            if (item.parent().valid() && !parent.valid()) {
                conflict = ConflictKind::MissingParent;
                return ok();
            }
            entity->parent = parent;
            return ok();
        }
        default:
            break;
    }
    return fail(ErrorCode::Internal, "unhandled override operation");
}

Status Resolver::apply_overrides(OverrideList& overrides, const IdMap& map, ValueSource source,
                                 AssetId owner, LocalId instance, ResolvedGraph& out) noexcept {
    for (usize index = 0; index < overrides.size(); ++index) {
        Override& item = overrides[index];

        // Migrate the target before applying it: an override authored against an older schema
        // addresses the identifier that schema used, and the chain is what moves it forward.
        if (options_->schemas != nullptr && item.target().field.valid()) {
            OverrideTarget target = item.target();
            if (Status migrated = options_->schemas->migrate_field_id(
                    target.component, item.schema_version(), target.field);
                !migrated) {
                return migrated;
            }
            item.set_target(target);
        }

        ConflictKind conflict = ConflictKind::None;
        if (Status applied = apply_one(item, map, source, owner, out, conflict); !applied) {
            return applied;
        }
        item.set_conflict(conflict);
        if (conflict != ConflictKind::None) {
            if (Status recorded = record_conflict(owner, instance, index, conflict, item.target());
                !recorded) {
                return recorded;
            }
            continue;
        }
        ++report_->overrides_applied;
    }
    return ok();
}

Status Resolver::copy_own_entities(const Document& document, ResolvedGraph& out) noexcept {
    for (const DocumentEntity& entity : document.entities()) {
        const Expected<ResolvedEntity*, Error> created = out.add(entity.id);
        if (!created) {
            return make_unexpected(created.error());
        }
        ResolvedEntity& target = **created;
        const Expected<TextRef, Error> name = out.intern(document.text(entity.name));
        if (!name) {
            return make_unexpected(name.error());
        }
        target.name = name.value();
        target.parent = entity.parent;
        target.motion = entity.motion;
        target.flatten = entity.flatten;
        target.origin = document.id;
        target.origin_local = entity.id;

        for (const ComponentData& component : entity.components()) {
            const Expected<ResolvedComponent*, Error> copy =
                ensure_resolved_component(target, component.type, out.allocator());
            if (!copy) {
                return make_unexpected(copy.error());
            }
            if (Status cloned = component.record.clone_into((*copy)->record); !cloned) {
                return cloned;
            }
            for (const serialize::FieldValue& value : (*copy)->record.fields()) {
                if (Status noted =
                        note_provenance(**copy, value.id, ValueSource::Base, document.id);
                    !noted) {
                    return noted;
                }
            }
        }
    }
    return ok();
}

Status Resolver::copy_own_parameters(const Document& document, ResolvedGraph& out) noexcept {
    for (const ExposedParameter& parameter : document.parameters()) {
        ResolvedParameter copy;
        copy.id = parameter.id;
        copy.wire = parameter.wire;
        copy.value = Array<u8>(out.allocator());
        copy.bindings = Array<ParameterBinding>(out.allocator());
        if (Status kept = copy.value.append(parameter.default_value().span()); !kept) {
            return kept;
        }
        copy.has_value = !parameter.default_value().empty();
        const Expected<TextRef, Error> name = out.intern(document.text(parameter.name));
        if (!name) {
            return make_unexpected(name.error());
        }
        copy.name = name.value();
        if (Status kept = copy.bindings.append(parameter.bindings().span()); !kept) {
            return kept;
        }
        if (Status added = out.parameters().push_back(std::move(copy)); !added) {
            return added;
        }
    }
    return ok();
}

Status Resolver::expand_base(Document& document, ResolvedGraph& out, u32& next_auto,
                             u32 depth) noexcept {
    ResolvedGraph base(out.allocator());
    if (Status resolved = run(document.base(), base, depth + 1); !resolved) {
        return resolved;
    }

    IdMap map(out.allocator());
    if (Status spliced =
            splice(base, document.base_mapping().span(), kNoLocalId, cy::Transform::identity(),
                   /*keep_together=*/false, out, next_auto, map);
        !spliced) {
        return spliced;
    }
    if (Status taken = take_parameters(base, map, out); !taken) {
        return taken;
    }
    // A variant's arguments are re-*defaults*: they change what the parameter means from here down,
    // and an instance of this variant may still override them. So the value is set and the bindings
    // are not written yet.
    if (Status applied = apply_arguments(document.base_arguments().span(), out); !applied) {
        return applied;
    }

    return apply_overrides(document.base_overrides(), map, ValueSource::Variant, document.id,
                           kNoLocalId, out);
}

Status Resolver::expand_instance(Document& document, Instance& instance, ResolvedGraph& out,
                                 u32& next_auto, u32 depth) noexcept {
    ResolvedGraph sub(out.allocator());
    if (Status resolved = run(instance.source, sub, depth); !resolved) {
        return resolved;
    }

    IdMap map(out.allocator());
    const bool packed = instance.cook_mode == CookMode::Packed;
    if (Status spliced = splice(sub, instance.mapping().span(), instance.parent, instance.transform,
                                packed, out, next_auto, map);
        !spliced) {
        return spliced;
    }

    // The sub-graph's parameters are this placement's to supply and then to consume.
    const usize inherited_first = out.parameters().size();
    if (Status taken = take_parameters(sub, map, out); !taken) {
        return taken;
    }
    if (Status applied = apply_arguments(instance.arguments().span(), out); !applied) {
        return applied;
    }
    // Parameters before overrides: setting a field directly is the more specific act, so it wins.
    if (Status written =
            write_bindings(out, inherited_first, out.parameters().size(), document.id, *report_);
        !written) {
        return written;
    }
    // A placement consumes the parameters it supplied: they are the source's interface, not the
    // container's, and re-exposing them would let a scene accidentally publish a prefab's knobs.
    while (out.parameters().size() > inherited_first) {
        out.parameters().pop_back();
    }

    ++report_->instances_expanded;
    return apply_overrides(instance.overrides(), map, ValueSource::Instance, document.id,
                           instance.id, out);
}

Status Resolver::run(AssetId id, ResolvedGraph& out, u32 depth) noexcept {
    Document* document = library_->find_mutable(id);
    if (document == nullptr) {
        return fail(ErrorCode::NotFound, "the document is not registered in this library");
    }
    out.source = id;
    out.kind = document->kind;
    report_->deepest_variant_chain = std::max(report_->deepest_variant_chain, depth);

    u32 next_auto = document->next_local_id();

    if (document->is_variant()) {
        if (document->kind != AssetKind::Prefab) {
            return fail(ErrorCode::InvalidArgument, "only a prefab may be a variant of another");
        }
        if (Status expanded = expand_base(*document, out, next_auto, depth); !expanded) {
            return expanded;
        }
    }
    if (Status copied = copy_own_entities(*document, out); !copied) {
        return copied;
    }
    if (Status copied = copy_own_parameters(*document, out); !copied) {
        return copied;
    }
    for (Instance& instance : document->instances()) {
        if (Status expanded = expand_instance(*document, instance, out, next_auto, depth);
            !expanded) {
            return expanded;
        }
    }
    report_->entities = static_cast<u32>(out.entities().size());
    return ok();
}

}  // namespace

Status resolve(const Library& library, AssetId id, const ResolveOptions& options,
               ResolvedGraph& out, ResolveReport& report) noexcept {
    // A cycle makes resolution non-terminating, so it is checked before a single entity is created
    // rather than discovered by running out of stack.
    Array<AssetId> order(out.allocator());
    if (Status validated = library.dependency_order(id, order, report.chain); !validated) {
        return validated;
    }

    Resolver resolver(library, options, report);
    if (Status resolved = resolver.run(id, out, 0); !resolved) {
        return resolved;
    }

    // Whatever parameters are still exposed belong to the document that was asked for, so their
    // effective values are written into the graph. Resolving a prefab on its own therefore shows
    // what an instance with no arguments would get.
    const Expected<bool, Error> deep =
        library.variant_depth_exceeds_recommendation(id, report.chain);
    if (deep) {
        report.variant_depth_warning = deep.value();
    }

    if (Status written = write_bindings(out, 0, out.parameters().size(), id, report); !written) {
        return written;
    }
    report.entities = static_cast<u32>(out.entities().size());
    return ok();
}

Status populate_mapping(const Library& library, Document& container, Instance& instance,
                        const ResolveOptions& options) noexcept {
    ResolveReport report(container.allocator());
    ResolvedGraph sub(container.allocator());
    if (Status resolved = resolve(library, instance.source, options, sub, report); !resolved) {
        return resolved;
    }
    for (const ResolvedEntity& entity : sub.entities()) {
        if (instance.local_of(entity.id).valid()) {
            continue;
        }
        if (Status added = add_mapping(instance.mapping(), entity.id, container.allocate_id());
            !added) {
            return added;
        }
    }
    return ok();
}

Status populate_base_mapping(const Library& library, Document& variant,
                             const ResolveOptions& options) noexcept {
    if (!variant.is_variant()) {
        return fail(ErrorCode::InvalidArgument, "the document has no variant base");
    }
    ResolveReport report(variant.allocator());
    ResolvedGraph base(variant.allocator());
    if (Status resolved = resolve(library, variant.base(), options, base, report); !resolved) {
        return resolved;
    }
    for (const ResolvedEntity& entity : base.entities()) {
        if (mapped_local(variant.base_mapping().span(), entity.id).valid()) {
            continue;
        }
        if (Status added = add_mapping(variant.base_mapping(), entity.id, variant.allocate_id());
            !added) {
            return added;
        }
    }
    return ok();
}

Status diff(const ResolvedGraph& before, const ResolvedGraph& after,
            Array<GraphDifference>& out) noexcept {
    using Kind = GraphDifference::Kind;
    out.clear();

    for (const ResolvedEntity& old_entity : before.entities()) {
        const ResolvedEntity* now = after.find(old_entity.id);
        if (now == nullptr) {
            if (Status added = out.push_back(difference(Kind::EntityRemoved, old_entity.id));
                !added) {
                return added;
            }
            continue;
        }
        if (now->parent != old_entity.parent) {
            if (Status added = out.push_back(difference(Kind::ParentChanged, old_entity.id));
                !added) {
                return added;
            }
        }
        for (const ResolvedComponent& old_component : old_entity.components) {
            const ResolvedComponent* new_component =
                find_resolved_component(*now, old_component.type);
            if (new_component == nullptr) {
                if (Status added = out.push_back(
                        difference(Kind::ComponentRemoved, old_entity.id, old_component.type));
                    !added) {
                    return added;
                }
                continue;
            }
            for (const serialize::FieldValue& field : old_component.record.fields()) {
                const Span<const u8> was = old_component.record.bytes(field);
                const Span<const u8> now_bytes = new_component->record.bytes(field.id);
                const bool same =
                    was.size() == now_bytes.size() &&
                    (was.empty() || std::memcmp(was.data(), now_bytes.data(), was.size()) == 0);
                if (!same) {
                    if (Status added = out.push_back(difference(Kind::FieldChanged, old_entity.id,
                                                                old_component.type, field.id));
                        !added) {
                        return added;
                    }
                }
            }
        }
    }

    for (const ResolvedEntity& new_entity : after.entities()) {
        const ResolvedEntity* was = before.find(new_entity.id);
        if (was == nullptr) {
            if (Status added = out.push_back(difference(Kind::EntityAdded, new_entity.id));
                !added) {
                return added;
            }
            continue;
        }
        for (const ResolvedComponent& component : new_entity.components) {
            const ResolvedComponent* old_component = find_resolved_component(*was, component.type);
            if (old_component == nullptr) {
                if (Status added = out.push_back(
                        difference(Kind::ComponentAdded, new_entity.id, component.type));
                    !added) {
                    return added;
                }
                continue;
            }
            // A field the new graph sets and the old one did not. The first pass walks the old
            // record's fields, so it cannot see this one — and "the prefab now sets a field it used
            // to leave at its default" is exactly the edit a live update has to notice.
            for (const serialize::FieldValue& field : component.record.fields()) {
                if (!old_component->record.contains(field.id)) {
                    if (Status added = out.push_back(difference(Kind::FieldChanged, new_entity.id,
                                                                component.type, field.id));
                        !added) {
                        return added;
                    }
                }
            }
        }
    }
    return ok();
}

}  // namespace cy::scene::serialization
