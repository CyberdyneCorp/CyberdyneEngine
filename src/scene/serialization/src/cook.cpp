#include <cy/scene/serialization/cook.h>

#include <cstring>
#include <utility>

namespace cy::scene::serialization {
namespace {

/// The bias a cooked reference slot carries so that zero can mean "no entity".
inline constexpr u64 kReferenceBias = 1;

/// "This local id is not in the cooked graph." Distinct from index zero, which is a real entity.
inline constexpr u32 kNoTemplateIndex = 0xFFFF'FFFFU;

static_assert(
    sizeof(ecs::Entity) == serialize::kEntityReferenceBytes,
    "a cooked reference slot is the width of an ecs::Entity — see cooked.h, where layer 0 "
    "states the constant it cannot name the type of");

/// One archetype's worth of entities, as the emitter groups them.
struct BlockPlan {
    /// The component types, ascending. The archetype's identity for grouping purposes.
    Array<reflect::TypeId> types;
    /// Template indices of the entities in this block, ascending.
    Array<u32> rows;
};

[[nodiscard]] bool same_types(Span<const reflect::TypeId> left,
                              Span<const reflect::TypeId> right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (usize index = 0; index < left.size(); ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

/// The types of one entity that the layout table knows, ascending. Unknown components are counted
/// and left out: cooked data has no tags, so there is nowhere to carry one.
[[nodiscard]] Status known_types(const ResolvedEntity& entity, const ComponentLayoutTable& layouts,
                                 Array<reflect::TypeId>& out, u32& unknown) noexcept {
    out.clear();
    for (const ResolvedComponent& component : entity.components) {
        if (layouts.find(component.type) == nullptr) {
            ++unknown;
            continue;
        }
        usize index = 0;
        while (index < out.size() && out[index] < component.type) {
            ++index;
        }
        if (Status added = out.push_back(component.type); !added) {
            return added;
        }
        for (usize position = out.size() - 1; position > index; --position) {
            const reflect::TypeId moved = out[position - 1];
            out[position - 1] = out[position];
            out[position] = moved;
        }
    }
    return ok();
}

/// The world transform of `entity`, composed from its ancestors' local transforms.
[[nodiscard]] Expected<cy::Transform, Error> world_transform(const ResolvedGraph& graph,
                                                             const ResolvedEntity& entity,
                                                             const TransformBinding& binding,
                                                             u32 depth_limit) noexcept {
    cy::Transform accumulated = cy::Transform::identity();
    const ResolvedEntity* current = &entity;
    for (u32 depth = 0; depth <= depth_limit; ++depth) {
        const Expected<cy::Transform, Error> local = read_transform_of(*current, binding);
        if (!local) {
            return make_unexpected(local.error());
        }
        accumulated = local.value() * accumulated;
        if (!current->parent.valid()) {
            return accumulated;
        }
        const ResolvedEntity* parent = graph.find(current->parent);
        if (parent == nullptr) {
            return accumulated;  // A dangling parent link; the entity is a root in practice.
        }
        current = parent;
    }
    return fail(ErrorCode::OutOfRange, "the entity hierarchy is deeper than the cook allows");
}

/// True when the edge from `entity` to its parent has to exist at runtime.
///
/// The walk to the root, not a look at one edge. See the header: a static child of a static parent
/// still needs its relationship when anything above them moves.
[[nodiscard]] bool relationship_needed(const ResolvedGraph& graph, const ResolvedEntity& entity,
                                       u32 depth_limit) noexcept {
    if (entity.flatten == FlattenPolicy::Keep) {
        return true;
    }
    if (entity.flatten == FlattenPolicy::Flatten) {
        return false;
    }
    const ResolvedEntity* current = &entity;
    for (u32 depth = 0; depth <= depth_limit; ++depth) {
        if (current->motion == MotionKind::Dynamic) {
            return true;
        }
        if (!current->parent.valid()) {
            return false;
        }
        const ResolvedEntity* parent = graph.find(current->parent);
        if (parent == nullptr) {
            return false;
        }
        current = parent;
    }
    // A cycle in the parent links, which resolution cannot produce and a hand-built graph can.
    // Keeping the relationship is the conservative answer: it costs a `Parent` component, where
    // flattening would bake a transform derived from a walk that never terminated.
    return true;
}

}  // namespace

// --- ComponentLayoutTable
// -------------------------------------------------------------------------

Status ComponentLayoutTable::add(const reflect::TypeInfo& type,
                                 Span<const u32> entity_offsets) noexcept {
    if (!type.id.valid()) {
        return fail(ErrorCode::InvalidArgument, "a component layout addresses its type by TypeId");
    }
    if (find(type.id) != nullptr) {
        return fail(ErrorCode::AlreadyExists, "that component type already has a layout");
    }
    for (const u32 offset : entity_offsets) {
        if (offset + serialize::kEntityReferenceBytes > type.size) {
            return fail(ErrorCode::OutOfRange,
                        "a declared entity offset does not fit inside the component");
        }
    }

    ComponentLayout layout;
    layout.type = &type;
    layout.size = type.size;
    layout.alignment = type.alignment;
    layout.entity_offsets = Array<u32>(allocator());
    if (Status copied = layout.entity_offsets.append(entity_offsets); !copied) {
        return copied;
    }
    if (Status built = layout.fields.build(type); !built) {
        return built;
    }
    return layouts_.push_back(std::move(layout));
}

const ComponentLayout* ComponentLayoutTable::find(reflect::TypeId type) const noexcept {
    for (const ComponentLayout& layout : layouts_) {
        if (layout.type != nullptr && layout.type->id == type) {
            return &layout;
        }
    }
    return nullptr;
}

Status describe_from_world(const ecs::World& world, ComponentLayoutTable& out) noexcept {
    const ecs::ComponentRegistry& registry = world.components();
    for (ecs::ComponentTypeId id = 0; id < registry.size(); ++id) {
        const ecs::ComponentInfo& info = registry.info(id);
        if (info.type == nullptr || !info.type_id.valid()) {
            continue;  // A built-in keyed by name: `Parent` and `Children`, which the cook emits as
                       // relationships rather than as columns.
        }
        if (!ecs::kind_has_column(info.kind)) {
            continue;
        }
        if (Status added =
                out.add(*info.type, Span<const u32>(info.entity_offsets, info.entity_offset_count));
            !added) {
            return added;
        }
    }
    return ok();
}

u64 build_schema_of(const ComponentLayoutTable& layouts) noexcept {
    serialize::BuildSchemaDigest digest;
    for (const ComponentLayout& layout : layouts.layouts()) {
        digest.add_type(*layout.type, 0);
    }
    return digest.value();
}

// --- The pipeline
// ---------------------------------------------------------------------------------

Status validate_references(ResolvedGraph& graph, CookReport& report) noexcept {
    for (ResolvedEntity& entity : graph.entities()) {
        for (ResolvedComponent& component : entity.components) {
            for (usize index = 0; index < component.record.size(); ++index) {
                const serialize::FieldValue value = component.record.fields()[index];
                if (value.wire != serialize::WireType::LocalRef) {
                    continue;
                }
                const Expected<u32, Error> local = component.record.local_reference(value.id);
                if (!local) {
                    return make_unexpected(local.error());
                }
                if (local.value() != 0 && graph.find(LocalId(local.value())) != nullptr) {
                    continue;
                }
                ++report.dangling_references;
                if (Status nulled = component.record.set_local_reference(value.id, 0); !nulled) {
                    return nulled;
                }
            }
        }
    }
    return ok();
}

Status flatten_hierarchy(ResolvedGraph& graph, const TransformBinding& transform,
                         CookReport& report) noexcept {
    const u32 depth_limit = static_cast<u32>(graph.entities().size()) + 1;

    // Every world transform is computed before anything is written, because baking one entity's
    // transform changes what a later walk over its children would read.
    Array<cy::Transform> world(graph.allocator());
    Array<bool> flatten(graph.allocator());
    if (Status sized = world.resize(graph.entities().size()); !sized) {
        return sized;
    }
    if (Status sized = flatten.resize(graph.entities().size()); !sized) {
        return sized;
    }

    for (usize index = 0; index < graph.entities().size(); ++index) {
        const ResolvedEntity& entity = graph.entities()[index];
        if (transform.valid()) {
            const Expected<cy::Transform, Error> composed =
                world_transform(graph, entity, transform, depth_limit);
            if (!composed) {
                return make_unexpected(composed.error());
            }
            world[index] = composed.value();
        }
        flatten[index] = entity.parent.valid() && !relationship_needed(graph, entity, depth_limit);
    }

    for (usize index = 0; index < graph.entities().size(); ++index) {
        ResolvedEntity& entity = graph.entities()[index];
        if (!entity.parent.valid()) {
            continue;
        }
        if (!flatten[index]) {
            ++report.relationships_retained;
            continue;
        }
        if (transform.valid()) {
            if (Status baked = write_transform_of(entity, transform, world[index], entity.origin,
                                                  ValueSource::Cooked, graph.allocator());
                !baked) {
                return baked;
            }
        }
        entity.parent = kNoLocalId;
        ++report.relationships_flattened;
    }
    return ok();
}

Status cook(const Library& library, AssetId id, const CookOptions& options, CookedAsset& out,
            CookReport& report) noexcept {
    ResolvedGraph graph(out.allocator());
    if (Status resolved = resolve(library, id, options.resolve, graph, report.resolve); !resolved) {
        return resolved;
    }
    out.source = id;
    const Document* document = library.find(id);
    out.kind = (document != nullptr) ? document->kind : AssetKind::Prefab;
    return cook_resolved(graph, options, out, report);
}

namespace {

/// The template index of a local id, or `kNoTemplateIndex`.
///
/// The graph is in ascending local-id order and the template index of an entity is its position, so
/// this is a search over a sorted array. Linear rather than binary because the search key is the id
/// and the array holds ids in order — a binary search would be correct and would save nothing at
/// the entity counts one prefab or one authoring chunk holds.
[[nodiscard]] u32 template_index_of(Span<const LocalId> identity, LocalId local) noexcept {
    for (u32 index = 0; index < identity.size(); ++index) {
        if (identity[index] == local) {
            return index;
        }
    }
    return kNoTemplateIndex;
}

/// Step 5: assign persistent identities, and record the relationships and the provenance that
/// follow from them.
[[nodiscard]] Status assign_identities(const ResolvedGraph& graph, const CookOptions& options,
                                       Array<LocalId>& identity, CookedAsset& out) noexcept {
    for (const ResolvedEntity& entity : graph.entities()) {
        if (Status added = identity.push_back(entity.id); !added) {
            return added;
        }
    }
    out.entity_count = static_cast<u32>(graph.entities().size());

    for (u32 index = 0; index < graph.entities().size(); ++index) {
        const ResolvedEntity& entity = graph.entities()[index];
        const u32 parent = entity.parent.valid() ? template_index_of(identity.span(), entity.parent)
                                                 : kNoTemplateIndex;
        if (parent != kNoTemplateIndex) {
            if (Status added = out.relationships().push_back(TemplateRelationship{index, parent});
                !added) {
                return added;
            }
        }
        if (options.retain_provenance) {
            if (Status added =
                    out.origins().push_back(TemplateOrigin{entity.origin, entity.origin_local});
                !added) {
                return added;
            }
        }
    }
    return ok();
}

/// Group entities by their exact set of known component types — the archetype the runtime will put
/// them in — keeping both the groups and the rows within them in template-index order.
[[nodiscard]] Status plan_blocks(const ResolvedGraph& graph, const ComponentLayoutTable& layouts,
                                 Array<BlockPlan>& plans, CookReport& report) noexcept {
    Array<reflect::TypeId> types(plans.allocator());
    for (u32 index = 0; index < graph.entities().size(); ++index) {
        if (Status listed =
                known_types(graph.entities()[index], layouts, types, report.unknown_components);
            !listed) {
            return listed;
        }
        BlockPlan* plan = nullptr;
        for (BlockPlan& candidate : plans) {
            if (same_types(candidate.types.span(), types.span())) {
                plan = &candidate;
                break;
            }
        }
        if (plan == nullptr) {
            BlockPlan fresh{Array<reflect::TypeId>(plans.allocator()),
                            Array<u32>(plans.allocator())};
            if (Status copied = fresh.types.append(types.span()); !copied) {
                return copied;
            }
            if (Status added = plans.push_back(std::move(fresh)); !added) {
                return added;
            }
            plan = &plans.back();
        }
        if (Status added = plan->rows.push_back(index); !added) {
            return added;
        }
    }
    return ok();
}

/// Write the cook-time reference for one entity-typed field into a row's bytes.
///
/// A slot holds `template index + 1`, and zero means "no entity" — the bias is what makes a null
/// reference and a reference to the first entity distinguishable in a zero-initialised slot.
[[nodiscard]] Status write_reference_slot(const ComponentLayout& layout,
                                          const ResolvedComponent& component,
                                          Span<const LocalId> identity, u32 offset,
                                          u8* element) noexcept {
    const reflect::FieldInfo* field = nullptr;
    for (u32 position = 0; position < layout.type->field_count; ++position) {
        if (layout.type->fields[position].offset == offset) {
            field = &layout.type->fields[position];
            break;
        }
    }

    u64 slot = 0;
    if (field != nullptr) {
        const Expected<u32, Error> local = component.record.local_reference(field->id);
        if (local && local.value() != 0) {
            const u32 target = template_index_of(identity, LocalId(local.value()));
            if (target != kNoTemplateIndex) {
                slot = static_cast<u64>(target) + kReferenceBias;
            }
        }
    }
    std::memcpy(element + offset, &slot, sizeof(slot));
    return ok();
}

/// One column's bytes: one element per row of the plan, appended to `payload`.
[[nodiscard]] Status emit_column(const ResolvedGraph& graph, const BlockPlan& plan,
                                 const ComponentLayout& layout, reflect::TypeId type,
                                 Span<const LocalId> identity, Array<u8>& payload) noexcept {
    for (const u32 row : plan.rows) {
        const usize element = payload.size();
        if (Status sized = payload.resize(element + layout.size); !sized) {
            return sized;
        }
        std::memset(payload.data() + element, 0, layout.size);

        const ResolvedComponent* component = find_resolved_component(graph.entities()[row], type);
        if (component == nullptr) {
            continue;
        }
        if (Status written = serialize::record_to_object(component->record, layout.fields,
                                                         payload.data() + element);
            !written) {
            return written;
        }
        // The reference slots, written last because `record_to_object` deliberately does not touch
        // them: a local id is not an entity, and only a cook knows what it becomes.
        for (const u32 offset : layout.entity_offsets) {
            if (Status written = write_reference_slot(layout, *component, identity, offset,
                                                      payload.data() + element);
                !written) {
                return written;
            }
        }
    }
    return ok();
}

/// One archetype block: its columns, its reference sites, its row-index table and its payload.
[[nodiscard]] Status emit_block(const ResolvedGraph& graph, const BlockPlan& plan,
                                const ComponentLayoutTable& layouts, Span<const LocalId> identity,
                                serialize::CookedWriter& writer, Array<u8>& payload,
                                CookedAsset& out, CookReport& report) noexcept {
    payload.clear();
    serialize::CookedBlock block(out.allocator());
    block.set_row_count(static_cast<u32>(plan.rows.size()));

    for (usize column = 0; column < plan.types.size(); ++column) {
        const ComponentLayout* layout = layouts.find(plan.types[column]);
        if (layout == nullptr) {
            return fail(ErrorCode::Internal, "a planned column has no layout");
        }
        if (Status added = block.add_column(plan.types[column], layout->size); !added) {
            return added;
        }
        for (const u32 offset : layout->entity_offsets) {
            if (Status added = block.add_reference_site(static_cast<u16>(column), offset); !added) {
                return added;
            }
            ++report.reference_sites;
        }
    }

    // Grouping by archetype makes a block's rows non-consecutive template indices, so the map
    // between the two is written down rather than recomputed at spawn.
    for (const u32 row : plan.rows) {
        if (Status added = out.row_indices().push_back(row); !added) {
            return added;
        }
    }

    for (usize column = 0; column < plan.types.size(); ++column) {
        const ComponentLayout* layout = layouts.find(plan.types[column]);
        if (layout == nullptr) {
            return fail(ErrorCode::Internal, "a planned column has no layout");
        }
        if (Status written =
                emit_column(graph, plan, *layout, plan.types[column], identity, payload);
            !written) {
            return written;
        }
    }

    block.set_payload(payload.span());
    if (Status written = writer.write_block(block); !written) {
        return written;
    }
    report.payload_bytes += static_cast<u32>(payload.size());
    ++report.blocks;
    return ok();
}

}  // namespace

Status cook_resolved(ResolvedGraph& graph, const CookOptions& options, CookedAsset& out,
                     CookReport& report) noexcept {
    if (options.layouts == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a cook needs the component layouts it may emit");
    }
    if (options.fail_on_conflicts && !report.resolve.conflicts.empty()) {
        return fail(ErrorCode::InvalidArgument,
                    "the cook is configured to fail on unresolved override conflicts");
    }

    // 3. Validate references. 4. Flatten. Cycles were rejected before resolution ran.
    if (Status validated = validate_references(graph, report); !validated) {
        return validated;
    }
    if (Status flattened = flatten_hierarchy(graph, options.resolve.transform, report);
        !flattened) {
        return flattened;
    }

    // 5. Assign persistent identities. The graph is in ascending local-id order, so the template
    // index of an entity is its position — deterministic, and the same for two cooks of one input.
    Array<LocalId> identity(out.allocator());
    if (Status assigned = assign_identities(graph, options, identity, out); !assigned) {
        return assigned;
    }
    report.entities = out.entity_count;

    // 6. Emit.
    Array<BlockPlan> plans(out.allocator());
    if (Status planned = plan_blocks(graph, *options.layouts, plans, report); !planned) {
        return planned;
    }

    out.build_schema = build_schema_of(*options.layouts);
    serialize::CookedWriter writer(out.stream());
    if (Status begun = writer.begin_stream(out.build_schema); !begun) {
        return begun;
    }

    Array<u8> payload(out.allocator());
    for (const BlockPlan& plan : plans) {
        if (plan.types.empty() || plan.rows.empty()) {
            continue;
        }
        if (Status written = emit_block(graph, plan, *options.layouts, identity.span(), writer,
                                        payload, out, report);
            !written) {
            return written;
        }
    }

    return writer.end_stream();
}

}  // namespace cy::scene::serialization
