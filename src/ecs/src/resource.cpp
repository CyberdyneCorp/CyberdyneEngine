// The world's resources. Task 2.7.

#include <cy/ecs/resource.h>

#include <cstring>

namespace cy::ecs {
namespace {

[[nodiscard]] bool same_name(const char* left, const char* right) noexcept {
    if (left == nullptr || right == nullptr) {
        return false;
    }
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

}  // namespace

ResourceRegistry::~ResourceRegistry() {
    for (Slot& slot : slots_) {
        if (slot.storage != nullptr) {
            allocator_->deallocate(slot.storage, slot.size, slot.alignment);
            slot.storage = nullptr;
        }
    }
}

Expected<ResourceId, Error> ResourceRegistry::declare(const char* name, u32 size,
                                                      u32 alignment) noexcept {
    if (name == nullptr || name[0] == '\0' || size == 0) {
        return fail(ErrorCode::InvalidArgument, "a resource needs a name and a non-zero size");
    }
    for (usize index = 0; index < slots_.size(); ++index) {
        if (!same_name(slots_[index].name, name)) {
            continue;
        }
        if (slots_[index].size != size) {
            return fail(ErrorCode::AlreadyExists,
                        "a different resource is already declared under that name");
        }
        return static_cast<ResourceId>(index);
    }

    Slot slot;
    slot.name = name;
    slot.size = size;
    slot.alignment = (alignment == 0) ? 1 : alignment;
    slot.storage = allocator_->allocate(slot.size, slot.alignment);
    if (slot.storage == nullptr) {
        return fail(ErrorCode::OutOfMemory, "could not allocate a resource");
    }
    // Zeroed rather than left as whatever the allocator returned: a resource read before it is set
    // must be a defined value, because a system that reads one at startup is normal.
    std::memset(slot.storage, 0, slot.size);

    const auto id = static_cast<ResourceId>(slots_.size());
    if (Status pushed = slots_.push_back(slot); !pushed) {
        allocator_->deallocate(slot.storage, slot.size, slot.alignment);
        return make_unexpected(pushed.error());
    }
    return id;
}

Expected<ResourceId, Error> ResourceRegistry::declare(const reflect::TypeInfo& type) noexcept {
    Expected<ResourceId, Error> id = declare(type.name, type.size, type.alignment);
    if (id) {
        slots_[*id].type = type.id;
    }
    return id;
}

ResourceId ResourceRegistry::find(const char* name) const noexcept {
    for (usize index = 0; index < slots_.size(); ++index) {
        if (same_name(slots_[index].name, name)) {
            return static_cast<ResourceId>(index);
        }
    }
    return kInvalidResource;
}

ResourceId ResourceRegistry::find(reflect::TypeId type) const noexcept {
    for (usize index = 0; index < slots_.size(); ++index) {
        if (slots_[index].type == type) {
            return static_cast<ResourceId>(index);
        }
    }
    return kInvalidResource;
}

void* ResourceRegistry::get(ResourceId resource) noexcept {
    return (resource < slots_.size()) ? slots_[resource].storage : nullptr;
}

const void* ResourceRegistry::get(ResourceId resource) const noexcept {
    return (resource < slots_.size()) ? slots_[resource].storage : nullptr;
}

Status ResourceRegistry::set(ResourceId resource, const void* value, u32 size) noexcept {
    if (resource >= slots_.size()) {
        return fail(ErrorCode::NotFound, "no such resource");
    }
    if (size != slots_[resource].size) {
        return fail(ErrorCode::InvalidArgument, "the value is not the resource's size");
    }
    std::memcpy(slots_[resource].storage, value, size);
    return ok();
}

const char* ResourceRegistry::name_of(ResourceId resource) const noexcept {
    return (resource < slots_.size()) ? slots_[resource].name : "";
}

u32 ResourceRegistry::value_size(ResourceId resource) const noexcept {
    return (resource < slots_.size()) ? slots_[resource].size : 0;
}

}  // namespace cy::ecs
