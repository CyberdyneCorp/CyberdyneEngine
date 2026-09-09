// Apply and extract. See cy/scene/serialization/prefab_edit.h for the argument; this file carries
// the rules.

#include <cy/scene/serialization/prefab_edit.h>

#include <algorithm>
#include <utility>

namespace cy::scene::serialization {
namespace {

// --- Shared helpers -------------------------------------------------------------------------

/// Drop every mapping entry naming `source`. Backwards, so an erase does not move an entry past the
/// cursor.
void forget_mapping(Array<InstanceMapping>& mapping, LocalId source) noexcept {
    for (usize index = mapping.size(); index > 0; --index) {
        if (mapping[index - 1].source == source) {
            mapping.erase(index - 1);
        }
    }
}

/// Copy one entity's components into another entity, records and all.
[[nodiscard]] Status clone_components(const DocumentEntity& from, DocumentEntity& to) noexcept {
    for (const ComponentData& component : from.components()) {
        const Expected<ComponentData*, Error> slot = to.ensure(component.type);
        if (!slot) {
            return make_unexpected(slot.error());
        }
        if (Status copied = component.record.clone_into((*slot)->record); !copied) {
            return copied;
        }
    }
    return ok();
}

// --- Apply ---------------------------------------------------------------------------------

/// One apply in progress.
///
/// `added` is the reason this is a struct rather than four parameters: an `AddEntity` override
/// names an id the CONTAINER invented, the prefab gives the entity a different one, and every
/// override after it in the same list that addresses the container's id has to be redirected. The
/// pairs accumulate as the list is walked, which is why the walk is in order and why nothing here
/// is a pure function of one override.
struct Apply {
    Document& source;
    /// The placement's mapping, or a variant's base mapping. Written when an entity is added, and
    /// pruned when one is removed.
    Array<InstanceMapping>& mapping;
    /// Container id -> the prefab-local id this apply gave it.
    Array<InstanceMapping> added;
    ApplyReport& report;
};

/// The prefab-local id an override's target names: the remap when this apply created the entity,
/// and otherwise the id as authored, because an override addresses prefab-local identifiers.
[[nodiscard]] LocalId source_id_of(const Apply& apply, LocalId authored) noexcept {
    const LocalId remapped = mapped_local(apply.added.span(), authored);
    return remapped.valid() ? remapped : authored;
}

/// Remove an entity and everything under it — child entities and nested placements alike.
[[nodiscard]] u32 remove_subtree(Document& document, Array<InstanceMapping>& mapping,
                                 LocalId root) noexcept {
    u32 removed = 0;
    // Children first, so a grandchild is not orphaned by its parent going away underneath it.
    for (usize index = document.entities().size(); index > 0; --index) {
        const DocumentEntity& child = document.entities()[index - 1];
        if (child.parent == root) {
            removed += remove_subtree(document, mapping, child.id);
        }
    }
    for (usize index = document.instances().size(); index > 0; --index) {
        if (document.instances()[index - 1].parent == root) {
            document.instances().erase(index - 1);
        }
    }
    for (usize index = 0; index < document.entities().size(); ++index) {
        if (document.entities()[index].id == root) {
            document.entities().erase(index);
            forget_mapping(mapping, root);
            return removed + 1;
        }
    }
    return removed;
}

/// `AddEntity`: the prefab gains an entity, and the placement keeps the id the container gave it.
[[nodiscard]] Expected<ConflictKind, Error> apply_add_entity(Apply& apply,
                                                             const Override& item) noexcept {
    LocalId parent = kNoLocalId;
    if (item.parent().valid()) {
        parent = source_id_of(apply, item.parent());
        if (apply.source.find_entity(parent) == nullptr) {
            return ConflictKind::MissingParent;
        }
    }
    const Expected<DocumentEntity*, Error> entity = apply.source.add_entity(parent, "");
    if (!entity) {
        return make_unexpected(entity.error());
    }
    if (Status noted = add_mapping(apply.added, item.target().entity, (*entity)->id); !noted) {
        return make_unexpected(noted.error());
    }
    // The container already refers to this entity by the id the override named, so the placement
    // must go on giving it that id. Without this the next resolve would invent a fresh one and
    // every reference into the added entity would stop resolving.
    forget_mapping(apply.mapping, (*entity)->id);
    if (Status noted = add_mapping(apply.mapping, (*entity)->id, item.target().entity); !noted) {
        return make_unexpected(noted.error());
    }
    apply.report.entities_added += 1;
    return ConflictKind::None;
}

/// The four operations that address an entity that must already be in the prefab.
[[nodiscard]] Expected<ConflictKind, Error> apply_to_entity(Apply& apply, const Override& item,
                                                            DocumentEntity& entity) noexcept {
    switch (item.op()) {
        case OverrideOp::SetField: {
            ComponentData* component = entity.find(item.target().component);
            if (component == nullptr) {
                return ConflictKind::MissingComponent;
            }
            if (item.payload().find(item.target().field) == nullptr) {
                return ConflictKind::MissingField;
            }
            if (Status written = component->record.overlay(item.payload()); !written) {
                return make_unexpected(written.error());
            }
            apply.report.fields_set += 1;
            return ConflictKind::None;
        }
        case OverrideOp::AddComponent: {
            const Expected<ComponentData*, Error> component =
                entity.ensure(item.target().component);
            if (!component) {
                return make_unexpected(component.error());
            }
            if (Status written = (*component)->record.overlay(item.payload()); !written) {
                return make_unexpected(written.error());
            }
            apply.report.components_added += 1;
            return ConflictKind::None;
        }
        case OverrideOp::RemoveComponent:
            if (!entity.remove(item.target().component)) {
                return ConflictKind::MissingComponent;
            }
            apply.report.components_removed += 1;
            return ConflictKind::None;
        case OverrideOp::ReparentEntity: {
            LocalId parent = kNoLocalId;
            if (item.parent().valid()) {
                parent = source_id_of(apply, item.parent());
                if (apply.source.find_entity(parent) == nullptr) {
                    return ConflictKind::MissingParent;
                }
            }
            entity.parent = parent;
            apply.report.entities_reparented += 1;
            return ConflictKind::None;
        }
        default:
            break;
    }
    return fail(ErrorCode::Internal, "unhandled override operation");
}

[[nodiscard]] Expected<ConflictKind, Error> apply_one(Apply& apply, const Override& item) noexcept {
    if (item.op() == OverrideOp::AddEntity) {
        return apply_add_entity(apply, item);
    }
    const LocalId id = source_id_of(apply, item.target().entity);
    if (item.op() == OverrideOp::RemoveEntity) {
        if (apply.source.find_entity(id) == nullptr) {
            return ConflictKind::MissingEntity;
        }
        apply.report.entities_removed += remove_subtree(apply.source, apply.mapping, id);
        return ConflictKind::None;
    }
    DocumentEntity* entity = apply.source.find_entity(id);
    if (entity == nullptr) {
        return ConflictKind::MissingEntity;
    }
    return apply_to_entity(apply, item, *entity);
}

/// Walk one override list, writing what applies onto the source and marking what does not.
///
/// The list is walked forwards, because an `AddEntity` has to be seen before the overrides that
/// fill the entity it added; the removal pass is backwards, because that is the only direction in
/// which erasing from a list does not move the next element under the cursor.
[[nodiscard]] Status apply_list(Apply& apply, OverrideList& overrides) noexcept {
    for (Override& item : overrides) {
        const Expected<ConflictKind, Error> conflict = apply_one(apply, item);
        if (!conflict) {
            return make_unexpected(conflict.error());
        }
        item.set_conflict(*conflict);
        if (*conflict == ConflictKind::None) {
            apply.report.applied += 1;
        } else {
            apply.report.conflicted += 1;
        }
    }
    for (usize index = overrides.size(); index > 0; --index) {
        if (!overrides[index - 1].conflicted()) {
            (void)overrides.discard(index - 1);
        }
    }
    return ok();
}

// --- Extract -------------------------------------------------------------------------------

/// Every entity of the subtree rooted at `root`, ancestors before descendants.
///
/// Breadth-first over the array rather than recursive: an authoring document's hierarchy is data a
/// project supplies, and a deep one is exactly the input that turns recursion into a crash.
[[nodiscard]] Status collect_subtree(const Document& container, LocalId root,
                                     Array<LocalId>& out) noexcept {
    if (Status seeded = out.push_back(root); !seeded) {
        return seeded;
    }
    for (usize cursor = 0; cursor < out.size(); ++cursor) {
        const LocalId parent = out[cursor];
        for (const DocumentEntity& entity : container.entities()) {
            if (entity.parent == parent) {
                if (Status added = out.push_back(entity.id); !added) {
                    return added;
                }
            }
        }
    }
    return ok();
}

[[nodiscard]] bool contains_id(Span<const LocalId> ids, LocalId id) noexcept {
    return std::ranges::any_of(ids, [id](LocalId candidate) { return candidate == id; });
}

/// Copy the subtree's entities into the prefab, keeping every id. `root` loses its parent, because
/// a prefab's roots are roots.
[[nodiscard]] Status copy_entities(const Document& container, Span<const LocalId> subtree,
                                   LocalId root, Document& prefab) noexcept {
    for (const LocalId id : subtree) {
        const DocumentEntity* from = container.find_entity(id);
        if (from == nullptr) {
            return fail(ErrorCode::Internal, "the subtree named an entity the document lost");
        }
        const LocalId parent = id == root ? kNoLocalId : from->parent;
        const Expected<DocumentEntity*, Error> to =
            prefab.add_entity_with_id(id, parent, container.text(from->name));
        if (!to) {
            return make_unexpected(to.error());
        }
        (*to)->motion = from->motion;
        (*to)->flatten = from->flatten;
        if (Status copied = clone_components(*from, **to); !copied) {
            return copied;
        }
    }
    return ok();
}

/// Copy one nested placement into the prefab, mapping, overrides, arguments and all.
[[nodiscard]] Status copy_instance(const Document& container, const Instance& from,
                                   Document& prefab) noexcept {
    const Expected<Instance*, Error> to =
        prefab.add_instance_with_id(from.id, from.source, from.parent, container.text(from.name));
    if (!to) {
        return make_unexpected(to.error());
    }
    (*to)->transform = from.transform;
    (*to)->cook_mode = from.cook_mode;
    for (const InstanceMapping& entry : from.mapping()) {
        if (Status copied = add_mapping((*to)->mapping(), entry.source, entry.local); !copied) {
            return copied;
        }
    }
    for (const Override& item : from.overrides()) {
        Override clone(prefab.allocator());
        if (Status copied = item.clone_into(clone); !copied) {
            return copied;
        }
        if (Status added = (*to)->overrides().add(std::move(clone)); !added) {
            return added;
        }
    }
    for (const ParameterArgument& argument : from.arguments()) {
        ParameterArgument copy;
        copy.id = argument.id;
        copy.wire = argument.wire;
        copy.value = Array<u8>(prefab.allocator());
        if (Status sized = copy.value.resize(argument.value.size()); !sized) {
            return sized;
        }
        for (usize byte = 0; byte < argument.value.size(); ++byte) {
            copy.value[byte] = argument.value[byte];
        }
        if (Status added = (*to)->arguments().push_back(std::move(copy)); !added) {
            return added;
        }
    }
    return ok();
}

/// Move the nested placements that sit inside the subtree. Returns how many moved.
[[nodiscard]] Expected<u32, Error> move_instances(Document& container, Span<const LocalId> subtree,
                                                  Document& prefab) noexcept {
    u32 moved = 0;
    for (usize index = container.instances().size(); index > 0; --index) {
        const Instance& instance = container.instances()[index - 1];
        if (!contains_id(subtree, instance.parent)) {
            continue;
        }
        if (Status copied = copy_instance(container, instance, prefab); !copied) {
            return make_unexpected(copied.error());
        }
        container.instances().erase(index - 1);
        moved += 1;
    }
    return moved;
}

/// The placement the container gains: identity-mapped onto the ids the entities already had.
[[nodiscard]] Status place_extracted(Document& container, Span<const LocalId> subtree,
                                     LocalId parent, AssetId prefab_id, std::string_view name,
                                     ExtractReport& out) noexcept {
    const Expected<Instance*, Error> instance = container.add_instance(prefab_id, parent, name);
    if (!instance) {
        return make_unexpected(instance.error());
    }
    for (const LocalId id : subtree) {
        if (Status mapped = add_mapping((*instance)->mapping(), id, id); !mapped) {
            return mapped;
        }
    }
    out.instance = (*instance)->id;
    return ok();
}

}  // namespace

Status apply_instance_overrides(const Library& library, Document& container, Instance& instance,
                                ApplyReport& out) noexcept {
    if (container.find_instance(instance.id) != &instance) {
        return fail(ErrorCode::InvalidArgument, "the placement is not this document's");
    }
    Document* source = library.find_mutable(instance.source);
    if (source == nullptr) {
        return fail(ErrorCode::NotFound, "the placed document is not in the library");
    }
    Apply apply{*source, instance.mapping(), Array<InstanceMapping>(library.allocator()), out};
    return apply_list(apply, instance.overrides());
}

Status apply_variant_overrides(const Library& library, Document& variant,
                               ApplyReport& out) noexcept {
    if (!variant.is_variant()) {
        return fail(ErrorCode::InvalidArgument, "the document has no base to apply onto");
    }
    Document* source = library.find_mutable(variant.base());
    if (source == nullptr) {
        return fail(ErrorCode::NotFound, "the base document is not in the library");
    }
    Apply apply{*source, variant.base_mapping(), Array<InstanceMapping>(library.allocator()), out};
    return apply_list(apply, variant.base_overrides());
}

Status extract_prefab(Document& container, LocalId root, AssetId prefab_id, std::string_view name,
                      Document& prefab, ExtractReport& out) noexcept {
    const DocumentEntity* root_entity = container.find_entity(root);
    if (root_entity == nullptr) {
        return fail(ErrorCode::NotFound, "the subtree root is not an entity of this document");
    }
    if (prefab_id.is_nil() || prefab_id == container.id) {
        return fail(ErrorCode::InvalidArgument,
                    "an extracted prefab needs an identity of its own; a nil id or the "
                    "container's own would be an asset graph that names itself");
    }
    if (!prefab.entities().empty() || !prefab.instances().empty()) {
        return fail(ErrorCode::AlreadyExists, "the destination document already holds content");
    }

    const LocalId parent = root_entity->parent;
    Array<LocalId> subtree(container.allocator());
    if (Status collected = collect_subtree(container, root, subtree); !collected) {
        return collected;
    }

    prefab.kind = AssetKind::Prefab;
    prefab.id = prefab_id;
    prefab.schema_version = container.schema_version;
    // Above every id the container has issued, so a later edit to either document cannot issue an
    // id the other already used — which is what keeps the identity mapping below identity forever.
    prefab.set_next_local_id(container.next_local_id());

    if (Status copied = copy_entities(container, subtree.span(), root, prefab); !copied) {
        return copied;
    }
    const Expected<u32, Error> moved = move_instances(container, subtree.span(), prefab);
    if (!moved) {
        return make_unexpected(moved.error());
    }
    out.instances = *moved;
    out.entities = static_cast<u32>(subtree.size());

    for (const LocalId id : subtree) {
        for (usize index = 0; index < container.entities().size(); ++index) {
            if (container.entities()[index].id == id) {
                container.entities().erase(index);
                break;
            }
        }
    }
    return place_extracted(container, subtree.span(), parent, prefab_id, name, out);
}

}  // namespace cy::scene::serialization
