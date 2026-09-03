#pragma once
// Dense storage with a sparse index. Task 2.4.
//
// `core-memory-and-containers` — "Sequence containers": `SparseSet<T>` is dense storage with a
// sparse index, used for entity-keyed lookups. The shape is two arrays and a table: the values sit
// contiguously so iteration is linear, and the sparse table maps a key to its position so a lookup
// is one indirection.
//
// THE SPARSE TABLE IS FLAT, AND THAT IS A CHOICE. It is `max_key + 1` entries of `u32`, so a set
// holding ten entities with indices near a million costs four megabytes of table. That is the right
// trade for the caller this exists for — ECS storage, where keys are dense because the entity
// allocator reuses slots — and the wrong one for a sparse key space, which should use `HashMap`
// instead. `sparse_bytes()` reports the cost so the wrong choice is visible rather than invisible.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>

#include <utility>

namespace cy {

template <class T>
class SparseSet {
public:
    /// The sparse table's "no value here". A key this large is not addressable anyway.
    static constexpr u32 kNoPosition = 0xFFFFFFFFu;

    explicit SparseSet(Allocator& allocator = current_allocator()) noexcept
        : sparse_(allocator), keys_(allocator), values_(allocator) {}

    /// Insert or overwrite. Returns the stored value, or an allocation failure.
    template <class... Args>
    [[nodiscard]] Expected<T*, Error> emplace(u32 key, Args&&... args) noexcept {
        if (T* existing = find(key); existing != nullptr) {
            *existing = T(std::forward<Args>(args)...);
            return existing;
        }
        if (Status grown = grow_sparse_for(key); !grown) {
            return make_unexpected(grown.error());
        }
        Expected<T*, Error> slot = values_.emplace_back(std::forward<Args>(args)...);
        if (!slot) {
            return slot;
        }
        if (Status pushed = keys_.push_back(key); !pushed) {
            values_.pop_back();
            return make_unexpected(pushed.error());
        }
        sparse_[key] = static_cast<u32>(values_.size() - 1);
        return slot;
    }

    [[nodiscard]] Status insert(u32 key, const T& value) noexcept {
        Expected<T*, Error> slot = emplace(key, value);
        return slot ? ok() : Status{make_unexpected(slot.error())};
    }

    [[nodiscard]] T* find(u32 key) noexcept {
        const u32 position = position_of(key);
        return (position == kNoPosition) ? nullptr : &values_[position];
    }
    [[nodiscard]] const T* find(u32 key) const noexcept {
        const u32 position = position_of(key);
        return (position == kNoPosition) ? nullptr : &values_[position];
    }

    [[nodiscard]] bool contains(u32 key) const noexcept { return position_of(key) != kNoPosition; }

    /// Remove `key`. The last element takes its place, so removal is O(1) and the order changes.
    bool remove(u32 key) noexcept {
        const u32 position = position_of(key);
        if (position == kNoPosition) {
            return false;
        }
        const u32 last = static_cast<u32>(values_.size() - 1);
        if (position != last) {
            sparse_[keys_[last]] = position;
        }
        values_.remove_unordered(position);
        keys_.remove_unordered(position);
        sparse_[key] = kNoPosition;
        return true;
    }

    void clear() noexcept {
        for (u32 key : keys_) {
            sparse_[key] = kNoPosition;
        }
        keys_.clear();
        values_.clear();
    }

    [[nodiscard]] usize size() const noexcept { return values_.size(); }
    [[nodiscard]] bool empty() const noexcept { return values_.empty(); }

    /// The values, contiguously. This is what a loop over the set iterates.
    [[nodiscard]] Span<T> values() noexcept { return values_.span(); }
    [[nodiscard]] Span<const T> values() const noexcept { return values_.span(); }
    /// The key of each value, at the same index.
    [[nodiscard]] Span<const u32> keys() const noexcept { return keys_.span(); }

    [[nodiscard]] T* begin() noexcept { return values_.begin(); }
    [[nodiscard]] T* end() noexcept { return values_.end(); }

    /// What the sparse table costs. See the note at the top of this file.
    [[nodiscard]] usize sparse_bytes() const noexcept { return sparse_.size() * sizeof(u32); }

private:
    [[nodiscard]] u32 position_of(u32 key) const noexcept {
        return (key < sparse_.size()) ? sparse_[key] : kNoPosition;
    }

    [[nodiscard]] Status grow_sparse_for(u32 key) noexcept {
        if (key < sparse_.size()) {
            return ok();
        }
        const usize wanted = static_cast<usize>(key) + 1;
        if (Status reserved = sparse_.reserve(wanted); !reserved) {
            return reserved;
        }
        while (sparse_.size() < wanted) {
            if (Status pushed = sparse_.push_back(kNoPosition); !pushed) {
                return pushed;
            }
        }
        return ok();
    }

    Array<u32> sparse_;
    Array<u32> keys_;
    Array<T> values_;
};

}  // namespace cy
