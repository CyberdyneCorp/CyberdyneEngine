#include <cy/scene/serialization/overrides.h>

namespace cy::scene::serialization {

const char* override_op_name(OverrideOp op) noexcept {
    switch (op) {
        case OverrideOp::SetField:
            return "set_field";
        case OverrideOp::AddComponent:
            return "add_component";
        case OverrideOp::RemoveComponent:
            return "remove_component";
        case OverrideOp::AddEntity:
            return "add_entity";
        case OverrideOp::RemoveEntity:
            return "remove_entity";
        case OverrideOp::ReparentEntity:
            return "reparent_entity";
    }
    return "<invalid>";
}

const char* conflict_kind_name(ConflictKind kind) noexcept {
    switch (kind) {
        case ConflictKind::None:
            return "none";
        case ConflictKind::MissingEntity:
            return "missing_entity";
        case ConflictKind::MissingComponent:
            return "missing_component";
        case ConflictKind::MissingField:
            return "missing_field";
        case ConflictKind::MissingParent:
            return "missing_parent";
    }
    return "<invalid>";
}

const char* value_source_name(ValueSource source) noexcept {
    switch (source) {
        case ValueSource::Base:
            return "base";
        case ValueSource::Variant:
            return "variant";
        case ValueSource::Instance:
            return "instance";
        case ValueSource::Parameter:
            return "parameter";
        case ValueSource::Cooked:
            return "cooked";
    }
    return "<invalid>";
}

Status Override::clone_into(Override& out) const noexcept {
    out.op_ = op_;
    out.target_ = target_;
    out.parent_ = parent_;
    out.schema_version_ = schema_version_;
    out.conflict_ = conflict_;
    return payload_.clone_into(out.payload_);
}

bool OverrideList::discard(usize index) noexcept {
    if (index >= items_.size()) {
        return false;
    }
    items_.erase(index);
    return true;
}

usize OverrideList::conflict_count() const noexcept {
    usize count = 0;
    for (const Override& item : items_) {
        if (item.conflicted()) {
            ++count;
        }
    }
    return count;
}

}  // namespace cy::scene::serialization
