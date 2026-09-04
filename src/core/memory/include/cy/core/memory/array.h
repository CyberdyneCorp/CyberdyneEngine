#pragma once
// The sequence containers: `Array`, `FixedArray`, `SmallArray`, and `Span`. Task 2.4.
//
// `core-memory-and-containers` — "Sequence containers". `Array<T>` grows geometrically, supports
// `reserve`, and provides `remove_unordered` as an O(1) removal. `Span<T>` is a non-owning
// (pointer, length) view, and `std::span` is used where it is sufficient — which it is, so `Span`
// is an alias rather than a reimplementation.
//
// TWO PROPERTIES THAT SHAPE THE INTERFACE.
//
// 1. THERE IS NO COPY CONSTRUCTOR. "the copy SHALL be deep and explicit; the engine SHALL NOT use
//    implicit copy-on-write in runtime containers, so ownership and cost are visible at the call
//    site". A deep copy is `clone()`, it can fail, and it returns `Expected` — so a copy is a line
//    of code that says it is copying and admits it might not.
//
// 2. GROWTH RETURNS A STATUS. Under -fno-exceptions there is no other way for `push_back` to
//    report that the allocator refused. Every mutating operation that may allocate returns
//    `Status` or `Expected`, and `[[nodiscard]]` makes ignoring one a warning, which the engine
//    builds as an error.
//
// Allocation goes to the current allocator scope by default (`scope.h`), so a container constructed
// inside a subsystem's scope is attributed to that subsystem with no annotation at the call site.

#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/relocatable.h>
#include <cy/core/memory/scope.h>

#include <cstring>
#include <initializer_list>
#include <span>
#include <type_traits>
#include <utility>

namespace cy {

/// A non-owning view. `std::span` is sufficient, so this is its name rather than its replacement.
template <class T, usize Extent = std::dynamic_extent>
using Span = std::span<T, Extent>;

namespace detail {

/// Move `count` objects from `source` to `target`, leaving `source` empty of them.
///
/// One memcpy when `T` is trivially relocatable, which is the case the specification singles out;
/// otherwise move-construct and destroy, one at a time. Both are correct; the difference is that
/// the first is a single call into the C library and the second is `count` constructor calls.
template <class T>
void relocate(T* target, T* source, usize count) noexcept {
    if constexpr (is_trivially_relocatable_v<T>) {
        if (count != 0) {
            std::memcpy(static_cast<void*>(target), static_cast<const void*>(source),
                        count * sizeof(T));
        }
    } else {
        for (usize index = 0; index < count; ++index) {
            construct_at<T>(static_cast<void*>(target + index), std::move(source[index]));
            source[index].~T();
        }
    }
}

/// The same, for ranges that overlap — which is what closing a gap in the middle of an array is.
///
/// A separate function rather than making `relocate` use memmove: the growth path copies between
/// two distinct buffers and the specification asks for a single memcpy there, and memcpy on
/// overlapping ranges is undefined behaviour that AddressSanitizer reports and that a release build
/// silently gets wrong. Two names, each correct where it is used.
template <class T>
void relocate_overlapping(T* target, T* source, usize count) noexcept {
    if constexpr (is_trivially_relocatable_v<T>) {
        if (count != 0) {
            std::memmove(static_cast<void*>(target), static_cast<const void*>(source),
                         count * sizeof(T));
        }
    } else {
        // Forward, because the target is always before the source when a gap is being closed, so an
        // element is moved out before it is overwritten.
        for (usize index = 0; index < count; ++index) {
            construct_at<T>(static_cast<void*>(target + index), std::move(source[index]));
            source[index].~T();
        }
    }
}

template <class T>
void destroy_range(T* first, usize count) noexcept {
    if constexpr (!std::is_trivially_destructible_v<T>) {
        for (usize index = 0; index < count; ++index) {
            first[index].~T();
        }
    } else {
        (void)first;
        (void)count;
    }
}

/// The next capacity for a container of `current` that must hold `wanted`.
///
/// Doubling, with a floor: geometric growth is what makes a sequence of pushes amortised constant,
/// and the floor stops the first four pushes from being four allocations.
[[nodiscard]] constexpr usize grown_capacity(usize current, usize wanted) noexcept {
    usize capacity = (current < 8) ? 8 : current;
    while (capacity < wanted) {
        capacity *= 2;
    }
    return capacity;
}

}  // namespace detail

/// A growable, contiguous, allocator-aware sequence. The default dynamic array.
template <class T>
class Array {
public:
    using value_type = T;

    explicit Array(Allocator& allocator = current_allocator()) noexcept : allocator_(&allocator) {}

    ~Array() { release(); }

    Array(const Array&) = delete;
    Array& operator=(const Array&) = delete;

    Array(Array&& other) noexcept
        : allocator_(other.allocator_),
          data_(other.data_),
          size_(other.size_),
          capacity_(other.capacity_) {
        other.data_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
    }

    Array& operator=(Array&& other) noexcept {
        if (this != &other) {
            release();
            allocator_ = other.allocator_;
            data_ = other.data_;
            size_ = other.size_;
            capacity_ = other.capacity_;
            other.data_ = nullptr;
            other.size_ = 0;
            other.capacity_ = 0;
        }
        return *this;
    }

    /// The deep copy, named and fallible. See the note at the top of this file.
    [[nodiscard]] Expected<Array, Error> clone() const noexcept {
        Array copy(*allocator_);
        if (Status reserved = copy.reserve(size_); !reserved) {
            return make_unexpected(reserved.error());
        }
        for (usize index = 0; index < size_; ++index) {
            construct_at<T>(static_cast<void*>(copy.data_ + index), data_[index]);
        }
        copy.size_ = size_;
        return copy;
    }

    [[nodiscard]] Status reserve(usize wanted) noexcept {
        if (wanted <= capacity_) {
            return ok();
        }
        return reallocate_to(detail::grown_capacity(capacity_, wanted));
    }

    /// Shrink the allocation to exactly the current size. The container gives memory back, which is
    /// what a `Critical` pressure response needs from it.
    [[nodiscard]] Status shrink_to_fit() noexcept {
        if (size_ == capacity_) {
            return ok();
        }
        if (size_ == 0) {
            release();
            return ok();
        }
        return reallocate_to(size_);
    }

    [[nodiscard]] Status resize(usize count) noexcept
        requires std::is_default_constructible_v<T>
    {
        if (count < size_) {
            detail::destroy_range(data_ + count, size_ - count);
            size_ = count;
            return ok();
        }
        if (Status reserved = reserve(count); !reserved) {
            return reserved;
        }
        for (usize index = size_; index < count; ++index) {
            construct_at<T>(static_cast<void*>(data_ + index));
        }
        size_ = count;
        return ok();
    }

    [[nodiscard]] Status push_back(const T& value) noexcept {
        Expected<T*, Error> slot = emplace_back(value);
        return slot ? ok() : Status{make_unexpected(slot.error())};
    }

    [[nodiscard]] Status push_back(T&& value) noexcept {
        Expected<T*, Error> slot = emplace_back(std::move(value));
        return slot ? ok() : Status{make_unexpected(slot.error())};
    }

    template <class... Args>
    [[nodiscard]] Expected<T*, Error> emplace_back(Args&&... args) noexcept {
        if (size_ == capacity_) {
            if (Status grown = reallocate_to(detail::grown_capacity(capacity_, size_ + 1));
                !grown) {
                return make_unexpected(grown.error());
            }
        }
        return construct_at<T>(static_cast<void*>(data_ + size_++), std::forward<Args>(args)...);
    }

    [[nodiscard]] Status append(Span<const T> values) noexcept {
        if (Status reserved = reserve(size_ + values.size()); !reserved) {
            return reserved;
        }
        for (const T& value : values) {
            construct_at<T>(static_cast<void*>(data_ + size_++), value);
        }
        return ok();
    }

    void pop_back() noexcept {
        CY_ASSERT_MSG(size_ != 0, "pop_back() on an empty Array");
        if (size_ != 0) {
            data_[--size_].~T();
        }
    }

    /// O(1) removal: the last element takes the removed one's place. The order changes, which is
    /// the trade the name states.
    void remove_unordered(usize index) noexcept {
        CY_ASSERT_MSG(index < size_, "remove_unordered() past the end");
        if (index >= size_) {
            return;
        }
        data_[index].~T();
        --size_;
        if (index != size_) {
            detail::relocate(data_ + index, data_ + size_, 1);
        }
    }

    /// O(n) removal that keeps the order. Named `erase` rather than `remove` so that the cheap one
    /// is not reached for by accident.
    void erase(usize index) noexcept {
        CY_ASSERT_MSG(index < size_, "erase() past the end");
        if (index >= size_) {
            return;
        }
        data_[index].~T();
        --size_;
        detail::relocate_overlapping(data_ + index, data_ + index + 1, size_ - index);
    }

    void clear() noexcept {
        detail::destroy_range(data_, size_);
        size_ = 0;
    }

    [[nodiscard]] T& operator[](usize index) noexcept {
        CY_ASSERT_MSG(index < size_, "Array index out of range");
        return data_[index];
    }
    [[nodiscard]] const T& operator[](usize index) const noexcept {
        CY_ASSERT_MSG(index < size_, "Array index out of range");
        return data_[index];
    }

    [[nodiscard]] T& front() noexcept { return data_[0]; }
    [[nodiscard]] const T& front() const noexcept { return data_[0]; }
    [[nodiscard]] T& back() noexcept { return data_[size_ - 1]; }
    [[nodiscard]] const T& back() const noexcept { return data_[size_ - 1]; }

    [[nodiscard]] T* data() noexcept { return data_; }
    [[nodiscard]] const T* data() const noexcept { return data_; }
    [[nodiscard]] usize size() const noexcept { return size_; }
    [[nodiscard]] usize capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    [[nodiscard]] T* begin() noexcept { return data_; }
    [[nodiscard]] T* end() noexcept { return data_ + size_; }
    [[nodiscard]] const T* begin() const noexcept { return data_; }
    [[nodiscard]] const T* end() const noexcept { return data_ + size_; }

    [[nodiscard]] Span<T> span() noexcept { return Span<T>(data_, size_); }
    [[nodiscard]] Span<const T> span() const noexcept { return Span<const T>(data_, size_); }

    [[nodiscard]] Allocator& allocator() const noexcept { return *allocator_; }

private:
    [[nodiscard]] Status reallocate_to(usize new_capacity) noexcept {
        void* block = allocator_->allocate(new_capacity * sizeof(T), alignof(T));
        if (block == nullptr) {
            return fail(ErrorCode::OutOfMemory, "Array growth was refused by its allocator");
        }
        T* fresh = static_cast<T*>(block);
        detail::relocate(fresh, data_, size_);
        if (data_ != nullptr) {
            allocator_->deallocate(static_cast<void*>(data_), capacity_ * sizeof(T), alignof(T));
        }
        data_ = fresh;
        capacity_ = new_capacity;
        return ok();
    }

    void release() noexcept {
        detail::destroy_range(data_, size_);
        if (data_ != nullptr) {
            allocator_->deallocate(static_cast<void*>(data_), capacity_ * sizeof(T), alignof(T));
        }
        data_ = nullptr;
        size_ = 0;
        capacity_ = 0;
    }

    Allocator* allocator_;
    T* data_ = nullptr;
    usize size_ = 0;
    usize capacity_ = 0;
};

/// Inline capacity, no heap allocation, ever. `push_back` fails when it is full, which is the whole
/// contract: a `FixedArray` is what a hot path uses when the maximum is known and an allocation
/// would be a defect.
// NOLINTBEGIN(cppcoreguidelines-pro-type-member-init) — `storage_` is raw inline storage. Zeroing
// N * sizeof(T) bytes on construction is exactly the cost this container exists to avoid, and
// `size_` is what says which of those bytes hold objects.
template <class T, usize N>
class FixedArray {
public:
    using value_type = T;
    static constexpr usize kCapacity = N;

    FixedArray() noexcept = default;
    ~FixedArray() { clear(); }

    FixedArray(const FixedArray&) = delete;
    FixedArray& operator=(const FixedArray&) = delete;

    FixedArray(FixedArray&& other) noexcept {
        detail::relocate(data(), other.data(), other.size_);
        size_ = other.size_;
        other.size_ = 0;
    }

    FixedArray& operator=(FixedArray&& other) noexcept {
        if (this != &other) {
            clear();
            detail::relocate(data(), other.data(), other.size_);
            size_ = other.size_;
            other.size_ = 0;
        }
        return *this;
    }

    [[nodiscard]] Status push_back(const T& value) noexcept {
        Expected<T*, Error> slot = emplace_back(value);
        return slot ? ok() : Status{make_unexpected(slot.error())};
    }

    template <class... Args>
    [[nodiscard]] Expected<T*, Error> emplace_back(Args&&... args) noexcept {
        if (size_ == N) {
            return fail(ErrorCode::OutOfRange, "FixedArray is full");
        }
        return construct_at<T>(static_cast<void*>(data() + size_++), std::forward<Args>(args)...);
    }

    void pop_back() noexcept {
        if (size_ != 0) {
            data()[--size_].~T();
        }
    }

    void remove_unordered(usize index) noexcept {
        CY_ASSERT_MSG(index < size_, "remove_unordered() past the end");
        if (index >= size_) {
            return;
        }
        data()[index].~T();
        --size_;
        if (index != size_) {
            detail::relocate(data() + index, data() + size_, 1);
        }
    }

    void clear() noexcept {
        detail::destroy_range(data(), size_);
        size_ = 0;
    }

    [[nodiscard]] T& operator[](usize index) noexcept { return data()[index]; }
    [[nodiscard]] const T& operator[](usize index) const noexcept { return data()[index]; }

    // NOLINTBEGIN(bugprone-casting-through-void) — through void* on purpose: `storage_` is
    // `alignas(T)`, and a direct reinterpret_cast from `u8*` to `T*` is what -Wcast-align reports.
    // The engine builds with -Werror, so the compiler's opinion is the one that has to be met.
    [[nodiscard]] T* data() noexcept { return static_cast<T*>(static_cast<void*>(storage_)); }
    [[nodiscard]] const T* data() const noexcept {
        return static_cast<const T*>(static_cast<const void*>(storage_));
    }
    // NOLINTEND(bugprone-casting-through-void)
    [[nodiscard]] usize size() const noexcept { return size_; }
    [[nodiscard]] static constexpr usize capacity() noexcept { return N; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] bool full() const noexcept { return size_ == N; }

    [[nodiscard]] T* begin() noexcept { return data(); }
    [[nodiscard]] T* end() noexcept { return data() + size_; }
    [[nodiscard]] const T* begin() const noexcept { return data(); }
    [[nodiscard]] const T* end() const noexcept { return data() + size_; }
    [[nodiscard]] Span<T> span() noexcept { return Span<T>(data(), size_); }
    [[nodiscard]] Span<const T> span() const noexcept { return Span<const T>(data(), size_); }

private:
    // Deliberately uninitialised: it is storage for objects that have not been constructed yet, and
    // zeroing it would put a memset of the whole capacity in front of every construction.
    alignas(T) u8 storage_[N * sizeof(T)];
    usize size_ = 0;
};

/// Inline for `N` elements, spilling to the heap beyond. The shape most engine code wants: the
/// common case costs no allocation and the uncommon one still works.
// NOLINTEND(cppcoreguidelines-pro-type-member-init)

// NOLINTBEGIN(cppcoreguidelines-pro-type-member-init) — as above: `storage_` is raw inline storage.
template <class T, usize N>
class SmallArray {
public:
    using value_type = T;

    explicit SmallArray(Allocator& allocator = current_allocator()) noexcept
        : allocator_(&allocator), data_(inline_data()), capacity_(N) {}

    ~SmallArray() { release(); }

    SmallArray(const SmallArray&) = delete;
    SmallArray& operator=(const SmallArray&) = delete;

    SmallArray(SmallArray&& other) noexcept : allocator_(other.allocator_) {
        adopt(std::move(other));
    }

    SmallArray& operator=(SmallArray&& other) noexcept {
        if (this != &other) {
            release();
            allocator_ = other.allocator_;
            adopt(std::move(other));
        }
        return *this;
    }

    [[nodiscard]] Status reserve(usize wanted) noexcept {
        if (wanted <= capacity_) {
            return ok();
        }
        return grow_to(detail::grown_capacity(capacity_, wanted));
    }

    [[nodiscard]] Status push_back(const T& value) noexcept {
        Expected<T*, Error> slot = emplace_back(value);
        return slot ? ok() : Status{make_unexpected(slot.error())};
    }

    template <class... Args>
    [[nodiscard]] Expected<T*, Error> emplace_back(Args&&... args) noexcept {
        if (size_ == capacity_) {
            if (Status grown = grow_to(detail::grown_capacity(capacity_, size_ + 1)); !grown) {
                return make_unexpected(grown.error());
            }
        }
        return construct_at<T>(static_cast<void*>(data_ + size_++), std::forward<Args>(args)...);
    }

    void pop_back() noexcept {
        if (size_ != 0) {
            data_[--size_].~T();
        }
    }

    void remove_unordered(usize index) noexcept {
        CY_ASSERT_MSG(index < size_, "remove_unordered() past the end");
        if (index >= size_) {
            return;
        }
        data_[index].~T();
        --size_;
        if (index != size_) {
            detail::relocate(data_ + index, data_ + size_, 1);
        }
    }

    void clear() noexcept {
        detail::destroy_range(data_, size_);
        size_ = 0;
    }

    /// Whether the elements are still in the inline buffer. The question a caller asks when it is
    /// checking that a hot path did not allocate after all.
    [[nodiscard]] bool is_inline() const noexcept { return data_ == inline_data(); }

    [[nodiscard]] T& operator[](usize index) noexcept { return data_[index]; }
    [[nodiscard]] const T& operator[](usize index) const noexcept { return data_[index]; }
    [[nodiscard]] T* data() noexcept { return data_; }
    [[nodiscard]] const T* data() const noexcept { return data_; }
    [[nodiscard]] usize size() const noexcept { return size_; }
    [[nodiscard]] usize capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] T* begin() noexcept { return data_; }
    [[nodiscard]] T* end() noexcept { return data_ + size_; }
    [[nodiscard]] const T* begin() const noexcept { return data_; }
    [[nodiscard]] const T* end() const noexcept { return data_ + size_; }
    [[nodiscard]] Span<T> span() noexcept { return Span<T>(data_, size_); }
    [[nodiscard]] Span<const T> span() const noexcept { return Span<const T>(data_, size_); }

private:
    // NOLINTBEGIN(bugprone-casting-through-void) — see the note on FixedArray::data().
    [[nodiscard]] T* inline_data() noexcept {
        return static_cast<T*>(static_cast<void*>(storage_));
    }
    [[nodiscard]] const T* inline_data() const noexcept {
        return static_cast<const T*>(static_cast<const void*>(storage_));
    }
    // NOLINTEND(bugprone-casting-through-void)

    [[nodiscard]] Status grow_to(usize new_capacity) noexcept {
        void* block = allocator_->allocate(new_capacity * sizeof(T), alignof(T));
        if (block == nullptr) {
            return fail(ErrorCode::OutOfMemory, "SmallArray growth was refused by its allocator");
        }
        T* fresh = static_cast<T*>(block);
        detail::relocate(fresh, data_, size_);
        if (!is_inline()) {
            allocator_->deallocate(static_cast<void*>(data_), capacity_ * sizeof(T), alignof(T));
        }
        data_ = fresh;
        capacity_ = new_capacity;
        return ok();
    }

    /// Take `other`'s elements. A heap buffer is stolen by pointer; an inline one has to be
    /// relocated, because the buffer belongs to the object and not to the elements.
    void adopt(SmallArray&& other) noexcept {
        if (other.is_inline()) {
            data_ = inline_data();
            capacity_ = N;
            detail::relocate(data_, other.data_, other.size_);
        } else {
            data_ = other.data_;
            capacity_ = other.capacity_;
        }
        size_ = other.size_;
        other.data_ = other.inline_data();
        other.capacity_ = N;
        other.size_ = 0;
    }

    void release() noexcept {
        detail::destroy_range(data_, size_);
        if (!is_inline()) {
            allocator_->deallocate(static_cast<void*>(data_), capacity_ * sizeof(T), alignof(T));
        }
        data_ = inline_data();
        capacity_ = N;
        size_ = 0;
    }

    Allocator* allocator_;
    // Uninitialised for the same reason as FixedArray's. `data_` and `capacity_` carry defaults so
    // that a constructor which sets them in its body — the move constructor — never reads an
    // indeterminate value on the way there.
    alignas(T) u8 storage_[N * sizeof(T)];
    T* data_ = nullptr;
    usize size_ = 0;
    usize capacity_ = 0;
};
// NOLINTEND(cppcoreguidelines-pro-type-member-init)

}  // namespace cy
