#pragma once
// Resources: the world's named, typed singletons. Task 2.7.
//
// `ecs-core` — "Resources and singletons": the world holds named typed singleton values — time, an
// input snapshot, configuration, server handles — "accessed by systems through declared
// `Read`/`Write` access, so they participate in conflict detection like components".
//
// THE LAST CLAUSE IS THE REQUIREMENT. A resource is not a global with a nicer spelling; the point
// is that two systems writing one are *serialised by the scheduler*, which can only happen if a
// resource has a dense identifier the access set can carry. That is what `ResourceId` is, and it is
// why resources live in the world rather than in a process-wide table: a per-process singleton
// could not be declared per world, and the editor world and the play-mode world would share it.
//
// A resource is stored as bytes and handed back as a pointer. There is no type erasure machinery
// because there is nothing to dispatch: a resource is set once and read by systems that know its
// type. The reflected descriptor is recorded when there is one, so a diagnostic and a snapshot can
// name it.

#include <cy/core/base/expected.h>
#include <cy/core/jobs/access.h>
#include <cy/core/memory/array.h>
#include <cy/core/reflect/registry.h>

namespace cy::ecs {

/// A resource's index within one world. The same numbering `jobs::AccessSet::resource_read` and
/// `resource_write` carry — a separate identifier space from components, so resource 3 and
/// component 3 are different things and do not conflict with each other.
using ResourceId = jobs::ResourceId;

inline constexpr ResourceId kInvalidResource = 0xFFFF'FFFFu;

class ResourceRegistry {
public:
    explicit ResourceRegistry(Allocator& allocator) noexcept
        : allocator_(&allocator), slots_(allocator) {}

    ~ResourceRegistry();

    ResourceRegistry(const ResourceRegistry&) = delete;
    ResourceRegistry& operator=(const ResourceRegistry&) = delete;

    /// Declare a resource. Idempotent for an identical redeclaration, so two subsystems that both
    /// need the frame clock may both declare it. Zero-initialised until something sets it.
    [[nodiscard]] Expected<ResourceId, Error> declare(const char* name, u32 size,
                                                      u32 alignment) noexcept;

    /// Declare a reflected type as a resource. The name is the type's.
    [[nodiscard]] Expected<ResourceId, Error> declare(const reflect::TypeInfo& type) noexcept;

    template <class T>
    [[nodiscard]] Expected<ResourceId, Error> declare() noexcept {
        return declare(reflect::type_of<T>());
    }

    [[nodiscard]] ResourceId find(const char* name) const noexcept;
    [[nodiscard]] ResourceId find(reflect::TypeId type) const noexcept;

    [[nodiscard]] void* get(ResourceId resource) noexcept;
    [[nodiscard]] const void* get(ResourceId resource) const noexcept;

    template <class T>
    [[nodiscard]] T* get(ResourceId resource) noexcept {
        return static_cast<T*>(get(resource));
    }
    template <class T>
    [[nodiscard]] const T* get(ResourceId resource) const noexcept {
        return static_cast<const T*>(get(resource));
    }

    [[nodiscard]] Status set(ResourceId resource, const void* value, u32 size) noexcept;

    template <class T>
    [[nodiscard]] Status set(ResourceId resource, const T& value) noexcept {
        return set(resource, static_cast<const void*>(&value), static_cast<u32>(sizeof(T)));
    }

    [[nodiscard]] const char* name_of(ResourceId resource) const noexcept;
    /// The bytes this resource occupies. A snapshot reads it rather than storing the size twice.
    [[nodiscard]] u32 value_size(ResourceId resource) const noexcept;
    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(slots_.size()); }

private:
    struct Slot {
        const char* name = "";
        reflect::TypeId type;
        u32 size = 0;
        u32 alignment = 1;
        void* storage = nullptr;
    };

    Allocator* allocator_;
    Array<Slot> slots_;
};

}  // namespace cy::ecs
