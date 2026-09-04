#include <cy/core/serialize/migration.h>

namespace cy::serialize {

const char* migration_class_name(MigrationClass value) noexcept {
    switch (value) {
        case MigrationClass::Automatic:
            return "Automatic";
        case MigrationClass::Generated:
            return "Generated";
        case MigrationClass::Custom:
            return "Custom";
    }
    return "<invalid>";
}

const SchemaRegistry::Declaration* SchemaRegistry::find(reflect::TypeId type) const noexcept {
    for (const Declaration& entry : types_) {
        if (entry.type == type) {
            return &entry;
        }
    }
    return nullptr;
}

const SchemaRegistry::Step* SchemaRegistry::find_step(reflect::TypeId type,
                                                      u16 from_version) const noexcept {
    for (const Step& step : steps_) {
        if (step.type == type && step.from_version == from_version) {
            return &step;
        }
    }
    return nullptr;
}

Status SchemaRegistry::declare(reflect::TypeId type, u16 current_version) noexcept {
    if (!type.valid()) {
        return fail(ErrorCode::InvalidArgument,
                    "a schema declaration addresses its type by TypeId");
    }
    if (const Declaration* existing = find(type); existing != nullptr) {
        if (existing->current_version == current_version) {
            return ok();
        }
        return fail(ErrorCode::AlreadyExists,
                    "the type is already declared at a different schema version");
    }
    return types_.push_back(Declaration{type, current_version});
}

Status SchemaRegistry::add_migration(const Migration& migration) noexcept {
    if (!migration.type.valid()) {
        return fail(ErrorCode::InvalidArgument, "a migration addresses its type by TypeId");
    }
    if (migration.to_version != migration.from_version + 1) {
        return fail(ErrorCode::InvalidArgument,
                    "a migration advances the schema by exactly one version");
    }
    if (find_step(migration.type, migration.from_version) != nullptr) {
        return fail(ErrorCode::AlreadyExists,
                    "a migration from that version is already registered for this type");
    }

    const u32 offset = static_cast<u32>(remaps_.size());
    for (const FieldRemap& remap : migration.remaps) {
        if (!remap.from.valid() || !remap.to.valid()) {
            return fail(ErrorCode::InvalidArgument, "a field remap names two valid FieldIds");
        }
        if (Status appended = remaps_.push_back(remap); !appended) {
            return appended;
        }
    }

    Step step;
    step.type = migration.type;
    step.from_version = migration.from_version;
    step.to_version = migration.to_version;
    step.kind = migration.kind;
    step.name = migration.name;
    step.apply = migration.apply;
    step.remap_offset = offset;
    step.remap_count = static_cast<u32>(migration.remaps.size());
    return steps_.push_back(step);
}

Expected<u16, Error> SchemaRegistry::current_version(reflect::TypeId type) const noexcept {
    const Declaration* entry = find(type);
    if (entry == nullptr) {
        return fail(ErrorCode::NotFound, "no schema version is declared for this type");
    }
    return entry->current_version;
}

bool SchemaRegistry::declares(reflect::TypeId type) const noexcept {
    return find(type) != nullptr;
}

Status SchemaRegistry::migrate(ValueRecord& record, void* context) const noexcept {
    const Declaration* entry = find(record.type());
    if (entry == nullptr) {
        // An unknown type. Its data is preserved as it stands; migrating it would require knowing
        // what it means, and rejecting it would strip a disabled plugin's data from every file.
        return ok();
    }
    if (record.schema_version() > entry->current_version) {
        return fail(ErrorCode::Unsupported,
                    "serialized data is newer than this build's schema for the type");
    }

    while (record.schema_version() < entry->current_version) {
        const Step* step = find_step(record.type(), record.schema_version());
        if (step == nullptr) {
            return fail(ErrorCode::NotFound,
                        "no migration is registered from the data's schema version");
        }
        for (u32 index = 0; index < step->remap_count; ++index) {
            const FieldRemap& remap = remaps_[step->remap_offset + index];
            if (!record.contains(remap.from)) {
                continue;  // The field was absent, which is the same as having taken its default.
            }
            if (Status moved = record.retarget(remap.from, remap.to); !moved) {
                return moved;
            }
        }
        if (step->apply != nullptr) {
            if (Status applied = step->apply(record, context); !applied) {
                return applied;
            }
        }
        record.set_schema_version(step->to_version);
    }
    return ok();
}

Status SchemaRegistry::migrate_field_id(reflect::TypeId type, u16 from_version,
                                        reflect::FieldId& field) const noexcept {
    const Declaration* entry = find(type);
    if (entry == nullptr) {
        return ok();
    }
    if (from_version > entry->current_version) {
        return fail(ErrorCode::Unsupported,
                    "an override is newer than this build's schema for the type");
    }

    u16 version = from_version;
    while (version < entry->current_version) {
        const Step* step = find_step(type, version);
        if (step == nullptr) {
            return fail(ErrorCode::NotFound,
                        "no migration is registered from the override's schema version");
        }
        for (u32 index = 0; index < step->remap_count; ++index) {
            const FieldRemap& remap = remaps_[step->remap_offset + index];
            if (field == remap.from) {
                field = remap.to;
                break;
            }
        }
        version = step->to_version;
    }
    return ok();
}

}  // namespace cy::serialize
