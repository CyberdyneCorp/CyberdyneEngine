// Component registration. Task 2.2.

#include <cy/ecs/component.h>

#include <cy/ecs/buffer.h>

#include <bit>

namespace cy::ecs {
namespace {

[[nodiscard]] constexpr bool power_of_two(u32 value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

}  // namespace

const char* component_kind_name(ComponentKind kind) noexcept {
    switch (kind) {
        case ComponentKind::Data:
            return "data";
        case ComponentKind::Tag:
            return "tag";
        case ComponentKind::Shared:
            return "shared";
        case ComponentKind::Buffer:
            return "buffer";
        case ComponentKind::Sparse:
            return "sparse";
    }
    return "unknown";
}

u32 ComponentMask::count() const noexcept {
    u32 total = 0;
    for (const u64 value : words_) {
        total += static_cast<u32>(std::popcount(value));
    }
    return total;
}

u64 ComponentMask::hash() const noexcept {
    // FNV-1a over the words. The mask is the archetype table's key, so this only has to spread; it
    // is never persisted and nothing is derived from it.
    u64 hash = 1469598103934665603ULL;
    for (const u64 value : words_) {
        hash = (hash ^ value) * 1099511628211ULL;
    }
    return hash;
}

Expected<ComponentTypeId, Error> ComponentRegistry::register_reflected(
    const reflect::TypeInfo& type, const ComponentOptions& options) noexcept {
    if (!type.id.valid()) {
        return fail(ErrorCode::InvalidArgument,
                    "a component's reflected type must carry a manifest identifier");
    }
    if (!type.trivially_relocatable) {
        // Chunk compaction moves a row with memcpy and nothing else. A type whose move is not a
        // copy of its bytes would be corrupted by the first archetype transition, silently.
        return fail(ErrorCode::InvalidArgument,
                    "a component must be trivially relocatable: chunk compaction moves its bytes");
    }
    if (const ComponentTypeId* existing = by_type_.find(type.id.value()); existing != nullptr) {
        // Registering the same type twice is a module registered by two consumers, not an error.
        // Registering it under a different kind is, because the storage would disagree.
        if (infos_[*existing].kind != options.kind) {
            return fail(ErrorCode::AlreadyExists,
                        "this component type is already registered under a different kind");
        }
        return *existing;
    }

    ComponentInfo info;
    info.kind = options.kind;
    info.type = &type;
    info.type_id = type.id;
    info.name = type.name;
    info.alignment = (type.alignment == 0) ? 1 : type.alignment;
    info.value_size = type.size;
    return add(info, options);
}

Expected<ComponentTypeId, Error> ComponentRegistry::register_builtin(
    const char* name, u32 size, u32 alignment, const ComponentOptions& options) noexcept {
    if (name == nullptr || name[0] == '\0') {
        return fail(ErrorCode::InvalidArgument, "a built-in component needs a name");
    }
    if (const ComponentInfo* existing = find(name); existing != nullptr) {
        if (existing->kind != options.kind) {
            return fail(ErrorCode::AlreadyExists,
                        "this built-in component is already registered under a different kind");
        }
        return existing->id;
    }

    ComponentInfo info;
    info.kind = options.kind;
    info.name = name;
    info.alignment = (alignment == 0) ? 1 : alignment;
    info.value_size = size;
    return add(info, options);
}

Expected<ComponentTypeId, Error> ComponentRegistry::add(ComponentInfo info,
                                                        const ComponentOptions& options) noexcept {
    if (infos_.size() >= kMaxComponentTypes) {
        return fail(ErrorCode::OutOfRange,
                    "this world has registered kMaxComponentTypes component types");
    }
    if (!power_of_two(info.alignment)) {
        return fail(ErrorCode::InvalidArgument, "a component's alignment must be a power of two");
    }
    if (options.entity_offsets.size() > kMaxEntityRefsPerComponent) {
        return fail(ErrorCode::OutOfRange,
                    "a component declares at most kMaxEntityRefsPerComponent entity fields");
    }

    switch (info.kind) {
        case ComponentKind::Data:
            if (info.value_size == 0) {
                return fail(ErrorCode::InvalidArgument,
                            "a data component has a non-zero size; declare a zero-sized one as a "
                            "tag");
            }
            info.size = info.value_size;
            break;

        case ComponentKind::Tag:
            // Zero-sized: presence only, and presence lives in the archetype's mask. A tag
            // deliberately gets no column, which is what makes adding one cost a row move and
            // nothing else.
            info.size = 0;
            info.value_size = 0;
            break;

        case ComponentKind::Shared:
            if (info.value_size == 0) {
                return fail(ErrorCode::InvalidArgument, "a shared component has a non-zero size");
            }
            // The value is per archetype, not per row: entities sharing it are grouped into the
            // same chunks, which is the requirement's whole point.
            info.size = 0;
            break;

        case ComponentKind::Buffer:
            if (options.element_size == 0 || !power_of_two(options.element_alignment)) {
                return fail(ErrorCode::InvalidArgument,
                            "a buffer component needs an element size and a power-of-two element "
                            "alignment");
            }
            info.element_size = options.element_size;
            info.element_alignment = options.element_alignment;
            info.inline_capacity = options.inline_capacity;
            info.elements_are_entities = options.elements_are_entities;
            info.size = buffer_entry_size(options.element_size, options.element_alignment,
                                          options.inline_capacity);
            info.alignment = buffer_entry_alignment(options.element_alignment);
            info.value_size = info.size;
            break;

        case ComponentKind::Sparse:
            if (info.value_size == 0) {
                return fail(ErrorCode::InvalidArgument, "a sparse component has a non-zero size");
            }
            // A side table keyed by entity. No column, and therefore no archetype change when it
            // is added or removed — which is the reason to declare one.
            info.size = 0;
            break;
    }

    // The release hook exists for buffer components and no other kind: it is what frees a heap
    // spill when a row goes away, and a hook on a kind that owns nothing would be a per-row call
    // that does nothing.
    info.release = (info.kind == ComponentKind::Buffer) ? options.release : nullptr;
    info.entity_offset_count = static_cast<u32>(options.entity_offsets.size());
    for (u32 index = 0; index < info.entity_offset_count; ++index) {
        info.entity_offsets[index] = options.entity_offsets[index];
    }

    info.id = static_cast<ComponentTypeId>(infos_.size());
    if (Status pushed = infos_.push_back(info); !pushed) {
        return make_unexpected(pushed.error());
    }
    if (info.type_id.valid()) {
        Expected<ComponentTypeId*, Error> slot = by_type_.insert(info.type_id.value(), info.id);
        if (!slot) {
            infos_.pop_back();
            return make_unexpected(slot.error());
        }
    }
    return info.id;
}

const ComponentInfo* ComponentRegistry::find(reflect::TypeId type) const noexcept {
    const ComponentTypeId* found = by_type_.find(type.value());
    return (found == nullptr) ? nullptr : &infos_[*found];
}

const ComponentInfo* ComponentRegistry::find(const char* name) const noexcept {
    if (name == nullptr) {
        return nullptr;
    }
    for (const ComponentInfo& info : infos_) {
        const char* left = info.name;
        const char* right = name;
        while (*left != '\0' && *left == *right) {
            ++left;
            ++right;
        }
        if (*left == '\0' && *right == '\0') {
            return &info;
        }
    }
    return nullptr;
}

}  // namespace cy::ecs
