#include <cy/scene/serialization/spawn.h>

#include <cstring>
#include <utility>

namespace cy::scene::serialization {
namespace {

/// The bias a cooked reference slot carries. Mirrors cook.cpp; stated in both because the pair is
/// the whole encoding and a reader of either should not have to find the other.
inline constexpr u64 kReferenceBias = 1;

/// The byte offset of a field within its component, or nothing.
[[nodiscard]] const reflect::FieldInfo* field_at(const ComponentLayout& layout,
                                                 reflect::FieldId id) noexcept {
    return layout.fields.find(id);
}

}  // namespace

Status EntityTemplate::bind(const ecs::World& world, const ComponentLayoutTable& layouts,
                            const CookedAsset& asset) noexcept {
    blocks_.clear();
    asset_ = &asset;
    entity_count_ = asset.entity_count;

    serialize::CookedReader reader(asset.stream().data(), asset.stream().size());
    if (Status header = reader.read_header(build_schema_of(layouts)); !header) {
        return header;
    }

    serialize::CookedBlock block(blocks_.allocator());
    while (true) {
        Status read = reader.next_block(block);
        if (!read) {
            if (read.error().code == ErrorCode::NotFound) {
                break;
            }
            return read;
        }

        BoundBlock bound{Array<ecs::ComponentTypeId>(blocks_.allocator()),
                         Array<const void*>(blocks_.allocator()), Array<u32>(blocks_.allocator()),
                         Array<serialize::ReferenceSite>(blocks_.allocator()), block.row_count()};

        for (usize index = 0; index < block.columns().size(); ++index) {
            const serialize::CookedColumn& column = block.columns()[index];
            const ecs::ComponentInfo* info = world.components().find(column.type);
            if (info == nullptr) {
                return fail(ErrorCode::NotFound,
                            "the world has not registered a component the template carries");
            }
            if (info->size != column.element_size) {
                return fail(ErrorCode::InvalidArgument,
                            "a cooked column is not the width the world's component is");
            }
            if (Status added = bound.components.push_back(info->id); !added) {
                return added;
            }
            const Expected<Span<const u8>, Error> bytes = block.column_bytes(index);
            if (!bytes) {
                return make_unexpected(bytes.error());
            }
            if (Status added = bound.columns.push_back(bytes->data()); !added) {
                return added;
            }
            if (Status added = bound.element_sizes.push_back(column.element_size); !added) {
                return added;
            }
        }
        for (const serialize::ReferenceSite& site : block.reference_sites()) {
            if (Status added = bound.sites.push_back(site); !added) {
                return added;
            }
        }
        if (Status added = blocks_.push_back(std::move(bound)); !added) {
            return added;
        }
    }
    return ok();
}

Status EntityTemplate::instantiate_all(ecs::World& world, u32 count, Array<u8>& scratch,
                                       Array<ecs::Entity>& raw) const noexcept {
    for (const BoundBlock& block : blocks_) {
        if (block.rows == 0 || block.components.empty()) {
            continue;
        }

        // One replicated payload per block, so the whole batch is one instantiation. The columns
        // stay column-major and each is repeated `count` times, which puts the rows in
        // (instance, row) order — the order the entities come back in.
        Array<const void*> columns(scratch.allocator());
        usize total = 0;
        for (usize index = 0; index < block.components.size(); ++index) {
            total += static_cast<usize>(block.element_sizes[index]) * block.rows * count;
        }
        scratch.clear();
        if (Status sized = scratch.reserve(total); !sized) {
            return sized;
        }

        Array<usize> offsets(scratch.allocator());
        for (usize index = 0; index < block.components.size(); ++index) {
            if (Status noted = offsets.push_back(scratch.size()); !noted) {
                return noted;
            }
            const usize span = static_cast<usize>(block.element_sizes[index]) * block.rows;
            for (u32 instance = 0; instance < count; ++instance) {
                if (Status appended = scratch.append(
                        Span<const u8>(static_cast<const u8*>(block.columns[index]), span));
                    !appended) {
                    return appended;
                }
            }
        }
        for (const usize offset : offsets) {
            if (Status added = columns.push_back(scratch.data() + offset); !added) {
                return added;
            }
        }

        ecs::World::ArchetypeBlock request;
        request.components = block.components.span();
        request.columns = columns.span();
        request.count = block.rows * count;
        if (Status created = world.instantiate(request, raw); !created) {
            return created;
        }
    }
    return ok();
}

Status EntityTemplate::fix_up_references(ecs::World& world, u32 count, Span<const ecs::Entity> raw,
                                         Array<ecs::Entity>& out) const noexcept {
    // `raw` is in (block, instance, row) order; `out` is in (instance, template index) order. The
    // cook's row-index table is what maps between them, and it exists precisely because grouping by
    // archetype makes the two orders differ.
    usize cursor = 0;
    usize row_cursor = 0;
    for (const BoundBlock& block : blocks_) {
        if (block.rows == 0 || block.components.empty()) {
            continue;
        }
        for (u32 instance = 0; instance < count; ++instance) {
            for (u32 row = 0; row < block.rows; ++row) {
                const u32 template_index = asset_->row_indices()[row_cursor + row];
                out[(static_cast<usize>(instance) * entity_count_) + template_index] = raw[cursor];
                ++cursor;
            }
        }
        row_cursor += block.rows;
    }

    // A second pass rather than one fused into the first, so that a block with no reference sites —
    // which is most of them — costs nothing here at all.
    row_cursor = 0;
    for (const BoundBlock& block : blocks_) {
        if (block.rows == 0 || block.components.empty()) {
            continue;
        }
        for (const serialize::ReferenceSite& site : block.sites) {
            const ecs::ComponentTypeId component = block.components[site.column];
            for (u32 instance = 0; instance < count; ++instance) {
                const usize position = static_cast<usize>(instance) * entity_count_;
                for (u32 row = 0; row < block.rows; ++row) {
                    const u32 template_index = asset_->row_indices()[row_cursor + row];
                    void* element = world.get_mut(out[position + template_index], component);
                    if (element == nullptr) {
                        return fail(
                            ErrorCode::Internal,
                            "a spawned entity does not carry a component it was cooked with");
                    }
                    u64 slot = 0;
                    std::memcpy(&slot, static_cast<u8*>(element) + site.offset, sizeof(slot));
                    ecs::Entity target;
                    if (slot >= kReferenceBias) {
                        const u64 index = slot - kReferenceBias;
                        if (index < entity_count_) {
                            target = out[position + static_cast<usize>(index)];
                        }
                    }
                    std::memcpy(static_cast<u8*>(element) + site.offset, &target, sizeof(target));
                }
            }
        }
        row_cursor += block.rows;
    }
    return ok();
}

Status EntityTemplate::attach_relationships(ecs::World& world, u32 count,
                                            Span<const ecs::Entity> out) const noexcept {
    for (u32 instance = 0; instance < count; ++instance) {
        const usize base = static_cast<usize>(instance) * entity_count_;
        for (const TemplateRelationship& relationship : asset_->relationships()) {
            if (Status attached = world.set_parent(out[base + relationship.child],
                                                   out[base + relationship.parent]);
                !attached) {
                return attached;
            }
        }
    }
    return ok();
}

Status EntityTemplate::spawn_many(ecs::World& world, u32 count,
                                  Array<ecs::Entity>& out) const noexcept {
    if (asset_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the template has not been bound to a world");
    }
    if (count == 0 || entity_count_ == 0) {
        return ok();
    }

    const usize first = out.size();
    if (Status sized = out.resize(first + (static_cast<usize>(count) * entity_count_)); !sized) {
        return sized;
    }

    Array<ecs::Entity> raw(out.allocator());
    Array<u8> scratch(out.allocator());
    if (Status created = instantiate_all(world, count, scratch, raw); !created) {
        return created;
    }

    Array<ecs::Entity> placed(out.allocator());
    if (Status sized = placed.resize(static_cast<usize>(count) * entity_count_); !sized) {
        return sized;
    }
    if (Status fixed = fix_up_references(world, count, raw.span(), placed); !fixed) {
        return fixed;
    }
    if (Status attached = attach_relationships(world, count, placed.span()); !attached) {
        return attached;
    }
    for (usize index = 0; index < placed.size(); ++index) {
        out[first + index] = placed[index];
    }
    return ok();
}

Status EntityTemplate::spawn(ecs::World& world, Array<ecs::Entity>& out) const noexcept {
    return spawn_many(world, 1, out);
}

Status EntityTemplate::spawn_many(ecs::World& world, Span<const cy::Transform> placements,
                                  const TransformBinding& binding,
                                  Array<ecs::Entity>& out) const noexcept {
    if (!binding.valid()) {
        return fail(ErrorCode::InvalidArgument, "placing a batch needs a transform binding");
    }
    const usize first = out.size();
    if (Status spawned = spawn_many(world, static_cast<u32>(placements.size()), out); !spawned) {
        return spawned;
    }

    const ecs::ComponentInfo* info = world.components().find(binding.component);
    if (info == nullptr || info->type == nullptr) {
        return fail(ErrorCode::NotFound, "the transform component is not registered in this world");
    }
    const reflect::FieldInfo* field = info->type->find_field(binding.field);
    if (field == nullptr || field->size != sizeof(cy::Transform)) {
        return fail(ErrorCode::InvalidArgument,
                    "the bound transform field is not the width of a cy::Transform");
    }

    for (usize instance = 0; instance < placements.size(); ++instance) {
        const usize base = first + (instance * entity_count_);
        for (u32 index = 0; index < entity_count_; ++index) {
            const ecs::Entity entity = out[base + index];
            if (world.parent_of(entity).valid()) {
                continue;  // Only roots are placed; a child follows its parent.
            }
            void* element = world.get_mut(entity, info->id);
            if (element == nullptr) {
                continue;  // The entity carries no transform; nothing to place.
            }
            cy::Transform local;
            std::memcpy(&local, static_cast<u8*>(element) + field->offset, sizeof(local));
            const cy::Transform placed = placements[instance] * local;
            std::memcpy(static_cast<u8*>(element) + field->offset, &placed, sizeof(placed));
        }
    }
    return ok();
}

Status plan_live_update(const ResolvedGraph& before, const ResolvedGraph& after,
                        const ecs::World& world, const ComponentLayoutTable& layouts,
                        LiveUpdatePlan& out) noexcept {
    Array<GraphDifference> differences(out.updates().allocator());
    if (Status computed = diff(before, after, differences); !computed) {
        return computed;
    }

    for (const GraphDifference& change : differences) {
        switch (change.kind) {
            case GraphDifference::Kind::EntityAdded:
            case GraphDifference::Kind::EntityRemoved:
            case GraphDifference::Kind::ComponentAdded:
            case GraphDifference::Kind::ComponentRemoved:
            case GraphDifference::Kind::ParentChanged:
                // Structure changed. Reported before anything is applied, because "instances whose
                // state cannot be reconciled SHALL be reported rather than silently reset".
                out.set_requires_recreation(true);
                continue;
            case GraphDifference::Kind::FieldChanged:
                break;
        }

        const ComponentLayout* layout = layouts.find(change.component);
        if (layout == nullptr) {
            continue;  // A component this build does not have; nothing to write it into.
        }
        const reflect::FieldInfo* field = field_at(*layout, change.field);
        if (field == nullptr) {
            continue;
        }
        // Field classification decides, from the same declaration serialization reads.
        if (field->attributes.persistence == reflect::PersistenceKind::Derived) {
            ++out.recomputed_derived_fields;
            continue;
        }
        if (field->attributes.persistence != reflect::PersistenceKind::Authoring) {
            ++out.preserved_runtime_fields;
            continue;
        }

        const ResolvedEntity* entity = after.find(change.entity);
        if (entity == nullptr) {
            continue;
        }
        const ResolvedComponent* component = find_resolved_component(*entity, change.component);
        if (component == nullptr) {
            continue;
        }
        const ecs::ComponentInfo* info = world.components().find(change.component);
        if (info == nullptr) {
            continue;
        }

        u32 index = 0;
        for (const ResolvedEntity& candidate : after.entities()) {
            if (candidate.id == change.entity) {
                break;
            }
            ++index;
        }

        LiveFieldUpdate update;
        update.entity = index;
        update.component = info->id;
        update.offset = field->offset;
        update.size = field->size;
        update.bytes = Array<u8>(out.updates().allocator());
        // The value goes into the plan in the component's native layout, so applying it is a memcpy
        // and not a decode: a live edit lands during a frame, not during a load.
        Array<u8> element(out.updates().allocator());
        if (Status sized = element.resize(layout->size); !sized) {
            return sized;
        }
        std::memset(element.data(), 0, layout->size);
        if (Status written =
                serialize::record_to_object(component->record, layout->fields, element.data());
            !written) {
            return written;
        }
        if (Status kept =
                update.bytes.append(Span<const u8>(element.data() + field->offset, field->size));
            !kept) {
            return kept;
        }
        if (Status added = out.updates().push_back(std::move(update)); !added) {
            return added;
        }
    }
    return ok();
}

Status apply_live_update(ecs::World& world, const LiveUpdatePlan& plan,
                         Span<const ecs::Entity> instance) noexcept {
    if (plan.requires_recreation()) {
        return fail(ErrorCode::Unavailable,
                    "this edit changes structure; the policy must be reported and chosen before it "
                    "is applied");
    }
    for (const LiveFieldUpdate& update : plan.updates()) {
        if (update.entity >= instance.size()) {
            continue;
        }
        void* element = world.get_mut(instance[update.entity], update.component);
        if (element == nullptr) {
            continue;
        }
        std::memcpy(static_cast<u8*>(element) + update.offset, update.bytes.data(), update.size);
    }
    return ok();
}

}  // namespace cy::scene::serialization
