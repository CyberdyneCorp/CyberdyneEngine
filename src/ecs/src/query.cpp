// Query matching and its cache. Task 2.4.

#include <cy/ecs/query.h>

namespace cy::ecs {

Status QueryDesc::require(ComponentTypeId component) noexcept {
    if (component == kInvalidComponent) {
        return fail(ErrorCode::NotFound,
                    "a query term names a component this world has not "
                    "registered");
    }
    if (required_.test(component)) {
        return ok();
    }
    required_.set(component);
    return required_terms_.push_back(component);
}

Status QueryDesc::with(ComponentTypeId component) noexcept {
    return require(component);
}

Status QueryDesc::read(ComponentTypeId component) noexcept {
    if (Status required = require(component); !required) {
        return required;
    }
    // Declaring Read twice is an AlreadyExists from the access set, which is a copy-paste there and
    // an ordinary repeated term here — a query that mentions a component in two places still has
    // one access to it.
    Status declared = access_.read(component);
    return declared ? ok() : declared;
}

Status QueryDesc::write(ComponentTypeId component) noexcept {
    if (Status required = require(component); !required) {
        return required;
    }
    return access_.write(component);
}

Status QueryDesc::without(ComponentTypeId component) noexcept {
    if (component == kInvalidComponent) {
        return fail(ErrorCode::NotFound, "a without() term names an unregistered component");
    }
    if (excluded_.test(component)) {
        return ok();
    }
    excluded_.set(component);
    Status declared = access_.exclude(component);
    return declared ? ok() : declared;
}

Status QueryDesc::optional(ComponentTypeId component) noexcept {
    if (component == kInvalidComponent) {
        return fail(ErrorCode::NotFound, "an optional() term names an unregistered component");
    }
    for (const ComponentTypeId existing : optional_terms_) {
        if (existing == component) {
            return ok();
        }
    }
    if (Status pushed = optional_terms_.push_back(component); !pushed) {
        return pushed;
    }
    // An optional term is accessed when it is present, so it is a Read even though it does not
    // constrain matching. A system that reads it must have declared it, and this is the
    // declaration.
    Status declared = access_.read(component);
    return declared ? ok() : declared;
}

const void* QueryChunk::shared(ComponentTypeId component) const noexcept {
    for (const SharedValue& value : archetype_->shared()) {
        if (value.component == component) {
            return world_->shared_value(component, value.value);
        }
    }
    return nullptr;
}

bool Query::matches(const Archetype& archetype) const noexcept {
    if (!archetype.mask().contains(desc_.required())) {
        return false;
    }
    if (archetype.mask().intersects(desc_.excluded())) {
        return false;
    }
    const SharedValue& filter = desc_.shared_filter();
    if (filter.component == kInvalidComponent) {
        return true;
    }
    for (const SharedValue& value : archetype.shared()) {
        if (value.component == filter.component) {
            return value.value == filter.value;
        }
    }
    return false;
}

Status Query::refresh() noexcept {
    const u32 total = world_->archetypes().size();
    while (scanned_ < total) {
        const Archetype& archetype = world_->archetypes().at(scanned_);
        if (matches(archetype)) {
            if (Status pushed = matched_.push_back(scanned_); !pushed) {
                return pushed;
            }
        }
        ++scanned_;
    }
    return ok();
}

u64 Query::entity_count() noexcept {
    if (!refresh()) {
        return 0;
    }
    u64 total = 0;
    for (const u32 archetype_id : matched_) {
        total += world_->archetypes().at(archetype_id).entity_count();
    }
    return total;
}

}  // namespace cy::ecs
