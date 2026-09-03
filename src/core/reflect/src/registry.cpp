// The registry's storage. Task 1.1.1.
//
// A grown array of borrowed pointers. The engine's own containers and its allocator interface are
// the memory module's work at this same milestone (`core-memory-and-containers`); this module is
// beneath them in the dependency order and cannot use them, so it uses the fewest primitives that
// will do and will move onto the allocator interface when one exists above it.
//
// A failed allocation is an OutOfMemory Status, not a throw and not an abort: registration happens
// during startup, where a host that cannot proceed still deserves to say so.

#include <cy/core/reflect/registry.h>

#include <cy/core/reflect/control_plane.h>

#include <cstring>
#include <new>

namespace cy::reflect {
namespace {

bool same_string(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) {
        return a == b;
    }
    return std::strcmp(a, b) == 0;
}

}  // namespace

TypeRegistry::~TypeRegistry() {
    clear();
}

TypeRegistry::TypeRegistry(TypeRegistry&& other) noexcept
    : entries_(other.entries_), count_(other.count_), capacity_(other.capacity_) {
    other.entries_ = nullptr;
    other.count_ = 0;
    other.capacity_ = 0;
}

TypeRegistry& TypeRegistry::operator=(TypeRegistry&& other) noexcept {
    if (this != &other) {
        clear();
        entries_ = other.entries_;
        count_ = other.count_;
        capacity_ = other.capacity_;
        other.entries_ = nullptr;
        other.count_ = 0;
        other.capacity_ = 0;
    }
    return *this;
}

void TypeRegistry::clear() noexcept {
    delete[] entries_;
    entries_ = nullptr;
    count_ = 0;
    capacity_ = 0;
}

Status TypeRegistry::reserve(usize wanted) {
    if (wanted <= capacity_) {
        return ok();
    }
    usize capacity = (capacity_ == 0) ? 16 : capacity_ * 2;
    while (capacity < wanted) {
        capacity *= 2;
    }
    auto** grown = new (std::nothrow) const TypeInfo*[capacity];
    if (grown == nullptr) {
        return fail(ErrorCode::OutOfMemory, "TypeRegistry could not grow its entry table");
    }
    for (usize index = 0; index < count_; ++index) {
        grown[index] = entries_[index];
    }
    delete[] entries_;
    entries_ = grown;
    capacity_ = capacity;
    return ok();
}

Status TypeRegistry::add(const TypeInfo& info) {
    if (!info.id.valid()) {
        return fail(ErrorCode::InvalidArgument,
                    "a type registered with the null TypeId — the manifest assigns identifiers, "
                    "and zero is never assigned");
    }

    // Not find(): this is registration, not a reflected lookup, and counting it as one would make
    // startup look like a control-plane violation to anything measuring.
    for (usize index = 0; index < count_; ++index) {
        const TypeInfo* existing = entries_[index];
        if (existing->id != info.id) {
            continue;
        }
        if (existing == &info || same_string(existing->name, info.name)) {
            return ok();  // the same descriptor, or the same type registered twice
        }
        return fail(ErrorCode::AlreadyExists,
                    "two different types claim one TypeId — an identifier was reused, which is the "
                    "failure the identity manifest's tombstones exist to prevent");
    }

    if (auto grown = reserve(count_ + 1); !grown) {
        return grown;
    }
    entries_[count_] = &info;
    ++count_;
    return ok();
}

const TypeInfo* TypeRegistry::find(TypeId id) const noexcept {
    detail::note_reflected_lookup();
    for (usize index = 0; index < count_; ++index) {
        if (entries_[index]->id == id) {
            return entries_[index];
        }
    }
    return nullptr;
}

const TypeInfo* TypeRegistry::find(const char* name) const noexcept {
    detail::note_reflected_lookup();
    for (usize index = 0; index < count_; ++index) {
        if (same_string(entries_[index]->name, name)) {
            return entries_[index];
        }
    }
    return nullptr;
}

TypeRegistry& default_registry() {
    // Function-local, so it is constructed on first use rather than in an order the linker chose.
    static TypeRegistry registry;
    return registry;
}

}  // namespace cy::reflect
