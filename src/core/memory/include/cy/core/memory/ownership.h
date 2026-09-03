#pragma once
// The ownership vocabulary: `UniquePtr`, `Ref`, and the table that says which to use. Task 2.9.
//
// `core-memory-and-containers` — "Ownership conventions":
//
// | Form                       | Meaning                                                        |
// |----------------------------|----------------------------------------------------------------|
// | `UniquePtr<T>`             | Sole heap ownership                                            |
// | `Ref<T>`                   | Shared ownership of immutable-after-load data; deliberately rare |
// | `Handle<Tag>`              | Generational reference to server- or pool-owned objects         |
// | `Span<T>`, `StringView`    | Non-owning views with a caller-guaranteed lifetime             |
// | Arena and scratch pointers | Lifetime bounded by an arena or task scope                     |
//
// SHARED OWNERSHIP IS NOT THE DEFAULT OBJECT MODEL. `Ref<T>` is for data that is genuinely shared
// and immutable after load — a loaded mesh, a shader program, a font face — and NOT for per-entity
// gameplay state, which is component data owned by ECS storage. A subsystem that wants to observe
// an asset without keeping it alive holds an `AssetId` and resolves on demand; that is the point of
// asset ids being a distinct type from handles.
//
// COOKED ASSET DATA IS IMMUTABLE AFTER LOAD, so several workers read one loaded mesh with no
// synchronisation at all. `Ref`'s counter is atomic because the REFERENCES are shared across
// threads; the data behind it needs no synchronisation because nothing writes it. Mutable runtime
// state derived from an asset lives separately from the asset — an instance's state is not in the
// thing every instance shares.

#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/scope.h>

#include <atomic>
#include <type_traits>
#include <utility>

namespace cy {

/// Sole ownership of one heap object, released to the allocator it came from.
///
/// The allocator travels with the pointer rather than being a template parameter, so a `UniquePtr`
/// from an arena and one from the heap are the same type and can be stored in one container.
template <class T>
class UniquePtr {
public:
    UniquePtr() noexcept = default;
    UniquePtr(std::nullptr_t) noexcept {}

    UniquePtr(T* pointer, Allocator& allocator) noexcept
        : pointer_(pointer), allocator_(&allocator) {}

    ~UniquePtr() { reset(); }

    UniquePtr(const UniquePtr&) = delete;
    UniquePtr& operator=(const UniquePtr&) = delete;

    UniquePtr(UniquePtr&& other) noexcept : pointer_(other.pointer_), allocator_(other.allocator_) {
        other.pointer_ = nullptr;
    }

    UniquePtr& operator=(UniquePtr&& other) noexcept {
        if (this != &other) {
            reset();
            pointer_ = other.pointer_;
            allocator_ = other.allocator_;
            other.pointer_ = nullptr;
        }
        return *this;
    }

    void reset() noexcept {
        if (pointer_ != nullptr) {
            pointer_->~T();
            allocator_->deallocate(pointer_, sizeof(T), alignof(T));
            pointer_ = nullptr;
        }
    }

    /// Give up ownership without destroying. The caller takes on the release, and needs the
    /// allocator to do it — which is why `allocator()` stays readable afterwards.
    [[nodiscard]] T* release() noexcept {
        T* released = pointer_;
        pointer_ = nullptr;
        return released;
    }

    [[nodiscard]] T* get() const noexcept { return pointer_; }
    [[nodiscard]] T& operator*() const noexcept { return *pointer_; }
    [[nodiscard]] T* operator->() const noexcept { return pointer_; }
    explicit operator bool() const noexcept { return pointer_ != nullptr; }
    [[nodiscard]] Allocator* allocator() const noexcept { return allocator_; }

private:
    T* pointer_ = nullptr;
    Allocator* allocator_ = nullptr;
};

template <class T, class... Args>
[[nodiscard]] Expected<UniquePtr<T>, Error> make_unique(Allocator& allocator,
                                                        Args&&... args) noexcept {
    void* storage = allocator.allocate(sizeof(T), alignof(T));
    if (storage == nullptr) {
        return fail(ErrorCode::OutOfMemory, "make_unique was refused by its allocator");
    }
    return UniquePtr<T>(construct_at<T>(storage, std::forward<Args>(args)...), allocator);
}

/// The same, from the current allocator scope. A DIFFERENT NAME rather than an overload: an
/// overload taking `Args&&...` is viable for a call that passes an allocator as the first
/// constructor argument, and the compiler picks it, so the two spellings have to be told apart by
/// name and not by argument type.
template <class T, class... Args>
[[nodiscard]] Expected<UniquePtr<T>, Error> make_unique_scoped(Args&&... args) noexcept {
    return make_unique<T>(current_allocator(), std::forward<Args>(args)...);
}

/// The base of anything an engine `Ref` can point at.
///
/// The count is INTRUSIVE — in the object, not in a separate control block — so a `Ref` is one
/// pointer and a raw `T*` can be promoted back to a `Ref` by whoever owns the object. What happens
/// on the last release is a function pointer set once rather than a virtual destructor, so a
/// reference-counted type does not have to be polymorphic to be one.
class RefCounted {
public:
    /// Called on the last release, with the object about to be destroyed.
    using ReleaseFn = void (*)(RefCounted* object, Allocator* allocator) noexcept;

    RefCounted() noexcept = default;

    RefCounted(const RefCounted&) = delete;
    RefCounted& operator=(const RefCounted&) = delete;

    /// Say how this object is released. `make_ref` calls it; an object constructed some other way —
    /// one living in a memory-mapped asset page, say — leaves it unset, and then the last release
    /// decrements to zero and destroys nothing, which is the correct behaviour for storage the
    /// reference does not own.
    void set_release_policy(ReleaseFn release, Allocator* allocator) noexcept {
        release_ = release;
        allocator_ = allocator;
    }

    void add_ref() const noexcept { references_.fetch_add(1, std::memory_order_relaxed); }

    /// Drop a reference. True when this was the last one, in which case the object may already have
    /// been destroyed and the caller must not touch it again.
    bool release_ref() const noexcept {
        // Release on the way down, so every write made through any reference happens-before the
        // destruction. The acquire is a LOAD on the same atomic rather than a standalone fence: it
        // gives the last releaser the same synchronises-with relationship against every earlier
        // release, and GCC's ThreadSanitizer rejects `std::atomic_thread_fence` outright
        // (`-Werror=tsan`: "not supported with -fsanitize=thread"), which would put this module's
        // suites out of reach of the tool that is most likely to find a bug in it.
        if (references_.fetch_sub(1, std::memory_order_release) != 1) {
            return false;
        }
        (void)references_.load(std::memory_order_acquire);
        if (release_ != nullptr) {
            release_(const_cast<RefCounted*>(this), allocator_);
        }
        return true;
    }

    [[nodiscard]] u32 use_count() const noexcept {
        return references_.load(std::memory_order_relaxed);
    }

protected:
    // Not virtual, and not public: a `Ref` never destroys through this type, it destroys through
    // the release function, which knows the concrete type. Deleting a `RefCounted*` is therefore
    // impossible rather than merely wrong.
    ~RefCounted() = default;

private:
    mutable std::atomic<u32> references_{1};
    ReleaseFn release_ = nullptr;
    Allocator* allocator_ = nullptr;
};

/// Shared ownership of immutable-after-load data. Deliberately rare — see the note above.
template <class T>
class Ref {
public:
    Ref() noexcept = default;
    Ref(std::nullptr_t) noexcept {}

    /// Adopt a pointer whose count is already one — which is what `make_ref` returns. `retain()` is
    /// the spelling for a pointer that something else already owns.
    [[nodiscard]] static Ref adopt(T* pointer) noexcept {
        Ref reference;
        reference.pointer_ = pointer;
        return reference;
    }

    [[nodiscard]] static Ref retain(T* pointer) noexcept {
        if (pointer != nullptr) {
            pointer->add_ref();
        }
        return adopt(pointer);
    }

    ~Ref() { reset(); }

    Ref(const Ref& other) noexcept : pointer_(other.pointer_) {
        if (pointer_ != nullptr) {
            pointer_->add_ref();
        }
    }

    Ref& operator=(const Ref& other) noexcept {
        if (this != &other) {
            // Retain before release: two different `Ref`s to one object assigning to each other
            // would otherwise destroy it between the two statements.
            T* incoming = other.pointer_;
            if (incoming != nullptr) {
                incoming->add_ref();
            }
            reset();
            pointer_ = incoming;
        }
        return *this;
    }

    Ref(Ref&& other) noexcept : pointer_(other.pointer_) { other.pointer_ = nullptr; }

    Ref& operator=(Ref&& other) noexcept {
        if (this != &other) {
            reset();
            pointer_ = other.pointer_;
            other.pointer_ = nullptr;
        }
        return *this;
    }

    void reset() noexcept {
        if (pointer_ != nullptr) {
            (void)pointer_->release_ref();
            pointer_ = nullptr;
        }
    }

    [[nodiscard]] T* get() const noexcept { return pointer_; }
    [[nodiscard]] T& operator*() const noexcept { return *pointer_; }
    [[nodiscard]] T* operator->() const noexcept { return pointer_; }
    explicit operator bool() const noexcept { return pointer_ != nullptr; }
    [[nodiscard]] u32 use_count() const noexcept {
        return (pointer_ == nullptr) ? 0 : pointer_->use_count();
    }

    friend bool operator==(const Ref& a, const Ref& b) noexcept { return a.pointer_ == b.pointer_; }
    friend bool operator!=(const Ref& a, const Ref& b) noexcept { return a.pointer_ != b.pointer_; }

private:
    T* pointer_ = nullptr;
};

namespace detail {

/// The release function `make_ref` installs. A named template rather than a lambda, so that its
/// address is an ordinary function pointer and `RefCounted` needs no vtable.
template <class T>
void release_ref_counted(RefCounted* object, Allocator* allocator) noexcept {
    T* typed = static_cast<T*>(object);
    typed->~T();
    if (allocator != nullptr) {
        allocator->deallocate(typed, sizeof(T), alignof(T));
    }
}

}  // namespace detail

/// Construct a reference-counted object with a count of one.
template <class T, class... Args>
[[nodiscard]] Expected<Ref<T>, Error> make_ref(Allocator& allocator, Args&&... args) noexcept {
    static_assert(std::is_base_of_v<RefCounted, T>,
                  "a Ref<T> requires T to derive from cy::RefCounted — the count is intrusive");
    void* storage = allocator.allocate(sizeof(T), alignof(T));
    if (storage == nullptr) {
        return fail(ErrorCode::OutOfMemory, "make_ref was refused by its allocator");
    }
    T* object = construct_at<T>(storage, std::forward<Args>(args)...);
    object->set_release_policy(&detail::release_ref_counted<T>, &allocator);
    return Ref<T>::adopt(object);
}

/// The same, from the current allocator scope. Named rather than overloaded, for the reason given
/// on `make_unique_scoped`.
template <class T, class... Args>
[[nodiscard]] Expected<Ref<T>, Error> make_ref_scoped(Args&&... args) noexcept {
    return make_ref<T>(current_allocator(), std::forward<Args>(args)...);
}

}  // namespace cy
