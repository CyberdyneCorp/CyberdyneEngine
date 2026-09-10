// Attributes and modifiers. M8.b task 4.2.

#include <cy/gameplay/abilities/attributes.h>

#include <cstring>
#include <utility>

namespace cy::gameplay::abilities {
namespace {

constexpr u64 kFnvOffset = 1469598103934665603ULL;
constexpr u64 kFnvPrime = 1099511628211ULL;

[[nodiscard]] u64 mix(u64 hash, u64 value) noexcept {
    for (u32 byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8U)) & 0xFFULL;
        hash *= kFnvPrime;
    }
    return hash;
}

[[nodiscard]] u64 bits_of(f32 value) noexcept {
    u32 bits = 0;
    std::memcpy(static_cast<void*>(&bits), static_cast<const void*>(&value), sizeof(bits));
    return bits;
}

/// The class an operation belongs to, which is the outer key of the evaluation order.
[[nodiscard]] u8 op_class(ModifierOp op) noexcept {
    switch (op) {
        case ModifierOp::Add:
            return 0;
        case ModifierOp::Multiply:
            return 1;
        case ModifierOp::Custom:
            return 2;
        case ModifierOp::Override:
            return 3;
        case ModifierOp::ClampMin:
            return 4;
        case ModifierOp::ClampMax:
            return 5;
        case ModifierOp::Count:
            break;
    }
    return 0;
}

}  // namespace

const char* modifier_op_name(ModifierOp op) noexcept {
    switch (op) {
        case ModifierOp::Add:
            return "Add";
        case ModifierOp::Multiply:
            return "Multiply";
        case ModifierOp::Custom:
            return "Custom";
        case ModifierOp::Override:
            return "Override";
        case ModifierOp::ClampMin:
            return "ClampMin";
        case ModifierOp::ClampMax:
            return "ClampMax";
        case ModifierOp::Count:
            break;
    }
    return "Add";
}

AttributeSchema::AttributeSchema(Allocator& allocator) noexcept : declarations_(allocator) {}

Expected<AttributeId, Error> AttributeSchema::declare(
    const AttributeDeclaration& declaration) noexcept {
    if (declaration.stable_id == 0) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "an attribute's stable identity is never zero", 0});
    }
    if (find(declaration.stable_id) != kInvalidAttribute) {
        return make_unexpected(
            Error{ErrorCode::AlreadyExists, "that attribute identity is declared", 0});
    }
    if (Status pushed = declarations_.push_back(declaration); !pushed) {
        return make_unexpected(pushed.error());
    }
    return static_cast<AttributeId>(declarations_.size() - 1);
}

AttributeId AttributeSchema::find(u32 stable_id) const noexcept {
    for (usize index = 0; index < declarations_.size(); ++index) {
        if (declarations_[index].stable_id == stable_id) {
            return static_cast<AttributeId>(index);
        }
    }
    return kInvalidAttribute;
}

AttributeId AttributeSchema::find_by_name(Name name) const noexcept {
    for (usize index = 0; index < declarations_.size(); ++index) {
        if (declarations_[index].name == name) {
            return static_cast<AttributeId>(index);
        }
    }
    return kInvalidAttribute;
}

bool AttributeSchema::replicated(AttributeId id) const noexcept {
    return id < declarations_.size() && declarations_[id].replicated &&
           declarations_[id].persistence != PersistenceClass::Derived;
}

bool AttributeSchema::saved(AttributeId id) const noexcept {
    if (id >= declarations_.size()) {
        return false;
    }
    switch (declarations_[id].persistence) {
        case PersistenceClass::WorldPersistent:
        case PersistenceClass::ProfilePersistent:
        case PersistenceClass::SaveGame:
            return true;
        case PersistenceClass::SessionTransient:
        case PersistenceClass::Derived:
        case PersistenceClass::Count:
            break;
    }
    return false;
}

AttributeStore::AttributeStore(Allocator& allocator, const AttributeSchema& schema) noexcept
    : allocator_(&allocator),
      schema_(&schema),
      rows_(allocator),
      index_(allocator),
      customs_(allocator) {}

AttributeStore::Row* AttributeStore::find(ecs::Entity entity) noexcept {
    const u32* slot = index_.find(entity.bits());
    return slot != nullptr && *slot < rows_.size() ? &rows_[*slot] : nullptr;
}

const AttributeStore::Row* AttributeStore::find(ecs::Entity entity) const noexcept {
    const u32* slot = index_.find(entity.bits());
    return slot != nullptr && *slot < rows_.size() ? &rows_[*slot] : nullptr;
}

Status AttributeStore::add_entity(ecs::Entity entity) noexcept {
    if (find(entity) != nullptr) {
        return ok();
    }
    Row row{entity, Array<f32>(*allocator_), Array<f32>(*allocator_), Array<u8>(*allocator_),
            Array<ModifierRecord>(*allocator_)};
    const u32 count = schema_->count();
    if (Status sized = row.base.resize(count); !sized) {
        return sized;
    }
    if (Status sized = row.cached.resize(count); !sized) {
        return sized;
    }
    if (Status sized = row.dirty.resize(count); !sized) {
        return sized;
    }
    for (u32 index = 0; index < count; ++index) {
        row.base[index] = schema_->declaration(index).base;
        row.cached[index] = row.base[index];
        row.dirty[index] = 1;
    }
    if (Status pushed = rows_.push_back(std::move(row)); !pushed) {
        return pushed;
    }
    auto placed = index_.insert(entity.bits(), static_cast<u32>(rows_.size() - 1));
    if (!placed) {
        rows_.pop_back();
        return make_unexpected(placed.error());
    }
    return ok();
}

void AttributeStore::remove_entity(ecs::Entity entity) noexcept {
    const u32* slot = index_.find(entity.bits());
    if (slot == nullptr || *slot >= rows_.size()) {
        return;
    }
    // Swap-remove, then repoint whatever moved. Row order is not meaningful — `digest()` is
    // order-independent for exactly this reason — and an ordered erase would move every index the
    // table holds.
    const u32 position = *slot;
    (void)index_.remove(entity.bits());
    const auto last = static_cast<u32>(rows_.size() - 1);
    if (position != last) {
        rows_[position] = std::move(rows_[last]);
        if (u32* moved = index_.find(rows_[position].entity.bits()); moved != nullptr) {
            *moved = position;
        }
    }
    rows_.pop_back();
}

bool AttributeStore::has_entity(ecs::Entity entity) const noexcept {
    return find(entity) != nullptr;
}

Status AttributeStore::set_base(ecs::Entity entity, AttributeId attribute, f32 value) noexcept {
    Row* row = find(entity);
    if (row == nullptr || attribute >= row->base.size()) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such entity or attribute", 0});
    }
    row->base[attribute] = value;
    row->dirty[attribute] = 1;
    return ok();
}

f32 AttributeStore::base(ecs::Entity entity, AttributeId attribute) const noexcept {
    const Row* row = find(entity);
    return row != nullptr && attribute < row->base.size() ? row->base[attribute] : 0.0F;
}

bool AttributeStore::orders_before(const ModifierRecord& a, const ModifierRecord& b) noexcept {
    // (class, priority descending, source ordinal ascending, identity ascending). Every term is a
    // declared value: none of them is where the modifier happens to sit.
    const u8 left = op_class(a.modifier.op);
    const u8 right = op_class(b.modifier.op);
    if (left != right) {
        return left < right;
    }
    if (a.modifier.priority != b.modifier.priority) {
        return a.modifier.priority > b.modifier.priority;
    }
    if (a.modifier.source_ordinal != b.modifier.source_ordinal) {
        return a.modifier.source_ordinal < b.modifier.source_ordinal;
    }
    return a.id < b.id;
}

Expected<ModifierId, Error> AttributeStore::add_modifier(ecs::Entity entity,
                                                         const Modifier& modifier) noexcept {
    Row* row = find(entity);
    if (row == nullptr) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such entity", 0});
    }
    if (modifier.attribute >= row->base.size()) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such attribute", 0});
    }
    ModifierRecord record;
    record.id = next_modifier_++;
    record.modifier = modifier;
    if (Status pushed = row->modifiers.push_back(record); !pushed) {
        return make_unexpected(pushed.error());
    }
    // INSERTED IN ORDER, NOT APPENDED. The evaluation walk is then a forward pass and the order is
    // a property of the declaration rather than of the insertion.
    usize slot = row->modifiers.size() - 1;
    while (slot > 0 && orders_before(row->modifiers[slot], row->modifiers[slot - 1])) {
        std::swap(row->modifiers[slot], row->modifiers[slot - 1]);
        --slot;
    }
    row->dirty[modifier.attribute] = 1;
    return record.id;
}

bool AttributeStore::remove_modifier(ecs::Entity entity, ModifierId modifier) noexcept {
    Row* row = find(entity);
    if (row == nullptr) {
        return false;
    }
    for (usize index = 0; index < row->modifiers.size(); ++index) {
        if (row->modifiers[index].id != modifier) {
            continue;
        }
        const AttributeId attribute = row->modifiers[index].modifier.attribute;
        row->modifiers.erase(index);
        if (attribute < row->dirty.size()) {
            row->dirty[attribute] = 1;
        }
        return true;
    }
    return false;
}

u32 AttributeStore::remove_modifiers_of(ecs::Entity entity, u32 effect_instance) noexcept {
    Row* row = find(entity);
    if (row == nullptr) {
        return 0;
    }
    u32 removed = 0;
    usize index = row->modifiers.size();
    while (index-- > 0) {
        if (row->modifiers[index].modifier.effect_instance != effect_instance) {
            continue;
        }
        const AttributeId attribute = row->modifiers[index].modifier.attribute;
        row->modifiers.erase(index);
        if (attribute < row->dirty.size()) {
            row->dirty[attribute] = 1;
        }
        ++removed;
    }
    return removed;
}

u32 AttributeStore::modifier_count(ecs::Entity entity) const noexcept {
    const Row* row = find(entity);
    return row != nullptr ? static_cast<u32>(row->modifiers.size()) : 0;
}

Status AttributeStore::register_custom(Name name, CustomModifierFn function) noexcept {
    if (function == nullptr) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "a custom operation needs an implementation", 0});
    }
    for (Custom& custom : customs_) {
        if (custom.name == name) {
            custom.function = function;
            return ok();
        }
    }
    return customs_.push_back(Custom{name, function});
}

CustomModifierFn AttributeStore::custom_of(Name name) const noexcept {
    for (const Custom& custom : customs_) {
        if (custom.name == name) {
            return custom.function;
        }
    }
    return nullptr;
}

f32 AttributeStore::evaluate(const Row& row, AttributeId attribute, Contribution* out, u32 capacity,
                             u32& written) const noexcept {
    written = 0;
    f32 value = row.base[attribute];
    bool overridden = false;
    for (const ModifierRecord& record : row.modifiers) {
        if (record.modifier.attribute != attribute) {
            continue;
        }
        bool applied = true;
        switch (record.modifier.op) {
            case ModifierOp::Add:
                value += record.modifier.magnitude;
                break;
            case ModifierOp::Multiply:
                value *= record.modifier.magnitude;
                break;
            case ModifierOp::Custom: {
                const CustomModifierFn function = custom_of(record.modifier.custom);
                if (function != nullptr) {
                    value = function(value, record.modifier.magnitude);
                } else {
                    applied = false;
                }
                break;
            }
            case ModifierOp::Override:
                // The first Override the walk meets is the highest-priority one, because the list
                // is in tie-break order. The rest are recorded as not applied rather than dropped,
                // so an inspector can see what lost.
                if (!overridden) {
                    value = record.modifier.magnitude;
                    overridden = true;
                } else {
                    applied = false;
                }
                break;
            case ModifierOp::ClampMin:
                value = value < record.modifier.magnitude ? record.modifier.magnitude : value;
                break;
            case ModifierOp::ClampMax:
                value = value > record.modifier.magnitude ? record.modifier.magnitude : value;
                break;
            case ModifierOp::Count:
                applied = false;
                break;
        }
        if (out != nullptr && written < capacity) {
            out[written] = Contribution{record.id,
                                        record.modifier.op,
                                        record.modifier.magnitude,
                                        record.modifier.priority,
                                        record.modifier.source_ordinal,
                                        value,
                                        applied};
        }
        ++written;
    }
    const AttributeDeclaration& declaration = schema_->declaration(attribute);
    if (declaration.has_minimum && value < declaration.minimum) {
        value = declaration.minimum;
    }
    if (declaration.has_maximum && value > declaration.maximum) {
        value = declaration.maximum;
    }
    return value;
}

f32 AttributeStore::current(ecs::Entity entity, AttributeId attribute) const noexcept {
    const Row* row = find(entity);
    if (row == nullptr || attribute >= row->cached.size()) {
        return 0.0F;
    }
    if (row->dirty[attribute] == 0) {
        return row->cached[attribute];
    }
    u32 written = 0;
    const f32 value = evaluate(*row, attribute, nullptr, 0, written);
    row->cached[attribute] = value;
    row->dirty[attribute] = 0;
    return value;
}

u32 AttributeStore::explain(ecs::Entity entity, AttributeId attribute, Contribution* out,
                            u32 capacity) const noexcept {
    const Row* row = find(entity);
    if (row == nullptr || attribute >= row->base.size()) {
        return 0;
    }
    u32 written = 0;
    (void)evaluate(*row, attribute, out, capacity, written);
    return written;
}

u64 AttributeStore::digest() const noexcept {
    u64 hash = kFnvOffset;
    // Order-independent over entities — a set, whatever order they were added in — and ordered
    // within an entity, because an attribute's identity is its position in the schema.
    u64 accumulated = 0;
    for (const Row& row : rows_) {
        u64 entity_hash = mix(kFnvOffset, row.entity.bits());
        for (u32 attribute = 0; attribute < row.base.size(); ++attribute) {
            entity_hash = mix(entity_hash, bits_of(row.base[attribute]));
            entity_hash = mix(entity_hash, bits_of(current(row.entity, attribute)));
        }
        accumulated += entity_hash;
    }
    hash = mix(hash, accumulated);
    return mix(hash, rows_.size());
}

}  // namespace cy::gameplay::abilities
