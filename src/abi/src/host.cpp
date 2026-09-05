// The objects behind CyEngine and CyWorld. Tasks 2.4 and 2.6.

#include <cy/abi/host.h>

#include <cy/abi/errors.h>
#include <cy/core/base/assert.h>
#include <cy/core/memory/ownership.h>
#include <cy/ecs/world.h>

#include <cstring>

namespace cy::abi {

u32 var_type_storage_size(CyVarType type) noexcept {
    switch (type) {
        case CY_VAR_BOOL:
            return 1;
        case CY_VAR_F32:
            return 4;
        // Grouped rather than written one arm each: two arms with the same body are two chances for
        // one of them to be edited and the other forgotten.
        case CY_VAR_I64:
        case CY_VAR_F64:
        case CY_VAR_VEC2:
        case CY_VAR_ENTITY:
            return 8;
        case CY_VAR_VEC3:
            return 12;
        case CY_VAR_VEC4:
        case CY_VAR_QUAT:
            return 16;
        // Nil has no storage; a string and a byte block have no fixed width in a component, so a
        // field of one is a registration error rather than a value the engine would misread.
        case CY_VAR_NIL:
        case CY_VAR_STRING:
        case CY_VAR_BYTES:
            return 0;
    }
    return 0;
}

}  // namespace cy::abi

using cy::abi::ComponentRecord;
using cy::abi::FieldRecord;

CyWorld_T::CyWorld_T(cy::Allocator& abi_allocator, cy::ecs::World& ecs_world) noexcept
    : world(ecs_world), components(abi_allocator), fields(abi_allocator) {}

const ComponentRecord* CyWorld_T::find(const char* name) const noexcept {
    if (name == nullptr) {
        return nullptr;
    }
    for (const ComponentRecord& record : components) {
        if (std::strcmp(record.name, name) == 0) {
            return &record;
        }
    }
    return nullptr;
}

const ComponentRecord* CyWorld_T::record(CyComponentTypeId id) const noexcept {
    for (const ComponentRecord& entry : components) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

const FieldRecord* CyWorld_T::field(const ComponentRecord& component,
                                    cy::u32 index) const noexcept {
    if (index >= component.field_count) {
        return nullptr;
    }
    return &fields[component.field_first + index];
}

cy::Expected<CyComponentTypeId, cy::Error> CyWorld_T::register_component(
    const CyComponentTypeDesc& desc) noexcept {
    if (desc.name == nullptr || desc.name[0] == '\0') {
        return cy::fail(cy::ErrorCode::InvalidArgument, "a component type must be named");
    }
    if (desc.size == 0) {
        return cy::fail(cy::ErrorCode::InvalidArgument, "a component type has a non-zero size");
    }
    if (desc.alignment == 0 || (desc.alignment & (desc.alignment - 1)) != 0) {
        return cy::fail(cy::ErrorCode::InvalidArgument,
                        "a component type's alignment is a power of two");
    }

    // IDEMPOTENT BY NAME. A reloaded module registers its types again — that is step (f) of the
    // reload sequence — so a second registration of the same shape must hand back the same id
    // rather than fail. A registration that disagrees about the layout is a different type wearing
    // a used name, and that is refused.
    if (const ComponentRecord* existing = find(desc.name); existing != nullptr) {
        if (existing->size != desc.size) {
            return cy::fail(cy::ErrorCode::AlreadyExists,
                            "a component of that name is registered with a different size");
        }
        return existing->id;
    }

    // Fields are validated before anything is registered, so a rejected descriptor leaves the world
    // exactly as it was rather than half-registered.
    const auto first = static_cast<cy::u32>(fields.size());
    for (cy::u32 index = 0; index < desc.field_count; ++index) {
        const CyFieldDesc& field_desc = desc.fields[index];
        const auto type = static_cast<CyVarType>(field_desc.type);
        const cy::u32 width = cy::abi::var_type_storage_size(type);
        if (width == 0) {
            (void)fields.resize(first);
            return cy::fail(cy::ErrorCode::InvalidArgument,
                            "a component field has no fixed width in storage");
        }
        if (field_desc.size != width || field_desc.offset + width > desc.size) {
            (void)fields.resize(first);
            return cy::fail(cy::ErrorCode::InvalidArgument,
                            "a component field does not fit the component it belongs to");
        }
        FieldRecord record;
        record.name = (field_desc.name != nullptr) ? field_desc.name : "";
        record.type = type;
        record.offset = field_desc.offset;
        record.size = width;
        if (cy::Status pushed = fields.push_back(record); !pushed) {
            (void)fields.resize(first);
            return cy::make_unexpected(pushed.error());
        }
    }

    cy::Expected<cy::ecs::ComponentTypeId, cy::Error> registered =
        world.components().register_builtin(desc.name, desc.size, desc.alignment);
    if (!registered) {
        (void)fields.resize(first);
        return cy::make_unexpected(registered.error());
    }

    ComponentRecord record;
    record.id = registered.value();
    record.name = desc.name;
    record.size = desc.size;
    record.field_first = first;
    record.field_count = desc.field_count;
    if (cy::Status pushed = components.push_back(record); !pushed) {
        (void)fields.resize(first);
        return cy::make_unexpected(pushed.error());
    }
    return static_cast<CyComponentTypeId>(record.id);
}

CyEngine_T::CyEngine_T(cy::Allocator& abi_allocator) noexcept
    : allocator(abi_allocator), behaviours(abi_allocator) {}

CyEngine_T::~CyEngine_T() {
    for (cy::abi::BehaviourRecord* record : behaviours) {
        record->~CyBehaviourType_T();
        allocator.deallocate(static_cast<void*>(record), sizeof(cy::abi::BehaviourRecord),
                             alignof(cy::abi::BehaviourRecord));
    }
}

void CyEngine_T::abandon_generation() noexcept {
    cy::usize kept = 0;
    // Compacting in place: every record that survives is moved down to `kept`, which is never ahead
    // of the read position, so the array is rewritten from the front while it is being walked.
    for (CyBehaviourType_T* record : behaviours) {
        if (record->generation != generation) {
            behaviours[kept] = record;
            ++kept;
            continue;
        }
        record->~CyBehaviourType_T();
        allocator.deallocate(static_cast<void*>(record), sizeof(CyBehaviourType_T),
                             alignof(CyBehaviourType_T));
    }
    // Shrinking cannot fail: `resize` only reallocates when it grows.
    (void)behaviours.resize(kept);
    if (generation > 0) {
        --generation;
    }
}

cy::Expected<cy::abi::BehaviourRecord*, cy::Error> CyEngine_T::register_behaviour(
    const char* name, const CyBehaviourVTable& vtable) noexcept {
    if (name == nullptr || name[0] == '\0') {
        return cy::fail(cy::ErrorCode::InvalidArgument, "a behaviour type must be named");
    }
    if (vtable.struct_size == 0 || vtable.create == nullptr || vtable.destroy == nullptr) {
        return cy::fail(cy::ErrorCode::InvalidArgument,
                        "a behaviour vtable declares its size and at least create and destroy");
    }

    // COPY ONLY THE PREFIX BOTH SIDES AGREE ON. The module may have been compiled against a longer
    // vtable than this engine knows — that is what append-only growth means from the other
    // direction — and reading past what this build declares would read a field the engine has no
    // name for. Measured in the spike, in both directions.
    const auto copied = static_cast<cy::usize>(vtable.struct_size) < sizeof(CyBehaviourVTable)
                            ? static_cast<cy::usize>(vtable.struct_size)
                            : sizeof(CyBehaviourVTable);

    if (cy::abi::BehaviourRecord* existing = find_behaviour(name); existing != nullptr) {
        // The same generation registering the same name again: the module changed its mind before
        // any instance exists. Replacing the vtable is correct and re-registering is not an error.
        existing->vtable = CyBehaviourVTable{};
        std::memcpy(static_cast<void*>(&existing->vtable), static_cast<const void*>(&vtable),
                    copied);
        existing->vtable.struct_size = static_cast<cy::u32>(copied);
        return existing;
    }

    cy::Expected<cy::UniquePtr<cy::abi::BehaviourRecord>, cy::Error> allocated =
        cy::make_unique<cy::abi::BehaviourRecord>(allocator);
    if (!allocated) {
        return cy::make_unexpected(allocated.error());
    }
    if (cy::Status reserved = behaviours.reserve(behaviours.size() + 1); !reserved) {
        return cy::make_unexpected(reserved.error());
    }

    cy::abi::BehaviourRecord* record = allocated.value().release();
    record->name = name;
    record->generation = generation;
    std::memcpy(static_cast<void*>(&record->vtable), static_cast<const void*>(&vtable), copied);
    record->vtable.struct_size = static_cast<cy::u32>(copied);
    // The reserve above already succeeded, so this cannot fail; ignoring it here rather than
    // branching keeps the record's ownership in one place.
    (void)behaviours.push_back(record);
    return record;
}

cy::abi::BehaviourRecord* CyEngine_T::find_behaviour(const char* name) noexcept {
    if (name == nullptr) {
        return nullptr;
    }
    for (cy::abi::BehaviourRecord* record : behaviours) {
        if (record->generation == generation && std::strcmp(record->name, name) == 0) {
            return record;
        }
    }
    return nullptr;
}
