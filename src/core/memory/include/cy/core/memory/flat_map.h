#pragma once
// The two maps with a defined iteration order. Task 2.4.
//
// `core-memory-and-containers` — "Associative containers": `FlatMap<K, V>` is a sorted array, cache
// friendly for small maps; `OrderedMap<K, V>` is for where deterministic iteration order is
// required. And "Serialization order is deterministic": reflected data backed by a map is
// serialized through a container with a defined order, so the output is byte-stable across runs.
//
// THE TWO ORDERS ARE DIFFERENT AND BOTH ARE DEFINED.
//   `FlatMap` iterates in KEY order. Two maps with the same contents iterate identically whatever
//   order they were built in, which is what a serialized artefact wants.
//   `OrderedMap` iterates in INSERTION order. The order the program produced is preserved, which is
//   what a configuration file, a property list or an editor's field order wants.
//
// Neither is a `HashMap` with a promise attached: the order is a consequence of the storage, so it
// cannot decay when the hash seed changes.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>

#include <functional>
#include <utility>

namespace cy {

/// A sorted array of keys, and the values beside it. Lookup is a binary search; insertion is O(n)
/// because the arrays are kept sorted. That is the right trade under about a hundred entries, where
/// a contiguous scan of keys beats an open-addressed table's cache misses.
template <class K, class V, class Less = std::less<K>>
class FlatMap {
public:
    explicit FlatMap(Allocator& allocator = current_allocator()) noexcept
        : keys_(allocator), values_(allocator) {}

    [[nodiscard]] Status reserve(usize wanted) noexcept {
        if (Status reserved = keys_.reserve(wanted); !reserved) {
            return reserved;
        }
        return values_.reserve(wanted);
    }

    /// Insert, or overwrite the value of an existing key.
    [[nodiscard]] Expected<V*, Error> insert(const K& key, V value) noexcept {
        const usize position = lower_bound(key);
        if (position < keys_.size() && !Less{}(key, keys_[position])) {
            values_[position] = std::move(value);
            return &values_[position];
        }
        // Append then rotate into place: the arrays own their elements, so the shift has to be a
        // sequence of moves rather than a memmove over live objects.
        if (Status pushed = keys_.push_back(key); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (Status pushed = values_.push_back(std::move(value)); !pushed) {
            keys_.pop_back();
            return make_unexpected(pushed.error());
        }
        for (usize index = keys_.size() - 1; index > position; --index) {
            std::swap(keys_[index], keys_[index - 1]);
            std::swap(values_[index], values_[index - 1]);
        }
        return &values_[position];
    }

    [[nodiscard]] V* find(const K& key) noexcept {
        const usize position = lower_bound(key);
        if (position < keys_.size() && !Less{}(key, keys_[position])) {
            return &values_[position];
        }
        return nullptr;
    }
    [[nodiscard]] const V* find(const K& key) const noexcept {
        const usize position = lower_bound(key);
        if (position < keys_.size() && !Less{}(key, keys_[position])) {
            return &values_[position];
        }
        return nullptr;
    }

    [[nodiscard]] bool contains(const K& key) const noexcept { return find(key) != nullptr; }

    bool remove(const K& key) noexcept {
        const usize position = lower_bound(key);
        if (position >= keys_.size() || Less{}(key, keys_[position])) {
            return false;
        }
        keys_.erase(position);
        values_.erase(position);
        return true;
    }

    void clear() noexcept {
        keys_.clear();
        values_.clear();
    }

    [[nodiscard]] usize size() const noexcept { return keys_.size(); }
    [[nodiscard]] bool empty() const noexcept { return keys_.empty(); }
    /// The keys, in order. The values are at the same index.
    [[nodiscard]] Span<const K> keys() const noexcept { return keys_.span(); }
    [[nodiscard]] Span<V> values() noexcept { return values_.span(); }
    [[nodiscard]] Span<const V> values() const noexcept { return values_.span(); }

private:
    /// The first index whose key is not less than `key`.
    [[nodiscard]] usize lower_bound(const K& key) const noexcept {
        usize low = 0;
        usize high = keys_.size();
        while (low < high) {
            const usize middle = low + ((high - low) / 2);
            if (Less{}(keys_[middle], key)) {
                low = middle + 1;
            } else {
                high = middle;
            }
        }
        return low;
    }

    Array<K> keys_;
    Array<V> values_;
};

/// Insertion-ordered, with a hash index over it so lookup stays constant time.
///
/// Removal is O(1) and keeps the order of everything else by leaving a hole: the entry is marked
/// dead rather than shifted, and iteration skips it. `compact()` reclaims the holes, and
/// `hole_count()` says whether it is worth calling.
template <class K, class V, class Hasher = Hash<K>, class Equal = std::equal_to<K>>
class OrderedMap {
public:
    struct Entry {
        K key;
        V value;
        bool live = true;
    };

    explicit OrderedMap(Allocator& allocator = current_allocator()) noexcept
        : entries_(allocator), index_(allocator) {}

    [[nodiscard]] Status reserve(usize wanted) noexcept {
        if (Status reserved = entries_.reserve(wanted); !reserved) {
            return reserved;
        }
        return index_.reserve(wanted);
    }

    [[nodiscard]] Expected<V*, Error> insert(const K& key, V value) noexcept {
        if (usize* position = index_.find(key); position != nullptr) {
            entries_[*position].value = std::move(value);
            return &entries_[*position].value;
        }
        Expected<Entry*, Error> placed = entries_.emplace_back(Entry{key, std::move(value), true});
        if (!placed) {
            return make_unexpected(placed.error());
        }
        Expected<usize*, Error> indexed = index_.insert(key, entries_.size() - 1);
        if (!indexed) {
            entries_.pop_back();
            return make_unexpected(indexed.error());
        }
        return &(*placed)->value;
    }

    [[nodiscard]] V* find(const K& key) noexcept {
        usize* position = index_.find(key);
        return (position == nullptr) ? nullptr : &entries_[*position].value;
    }
    [[nodiscard]] const V* find(const K& key) const noexcept {
        const usize* position = index_.find(key);
        return (position == nullptr) ? nullptr : &entries_[*position].value;
    }

    [[nodiscard]] bool contains(const K& key) const noexcept { return index_.contains(key); }

    bool remove(const K& key) noexcept {
        usize* position = index_.find(key);
        if (position == nullptr) {
            return false;
        }
        entries_[*position].live = false;
        ++holes_;
        return index_.remove(key);
    }

    void clear() noexcept {
        entries_.clear();
        index_.clear();
        holes_ = 0;
    }

    /// Drop the holes and rebuild the index, keeping the insertion order of what remains.
    [[nodiscard]] Status compact() noexcept {
        if (holes_ == 0) {
            return ok();
        }
        Array<Entry> compacted(entries_.allocator());
        if (Status reserved = compacted.reserve(entries_.size() - holes_); !reserved) {
            return reserved;
        }
        for (Entry& entry : entries_) {
            if (entry.live) {
                if (Status moved = compacted.push_back(std::move(entry)); !moved) {
                    return moved;
                }
            }
        }
        entries_ = std::move(compacted);
        index_.clear();
        for (usize position = 0; position < entries_.size(); ++position) {
            if (Expected<usize*, Error> indexed = index_.insert(entries_[position].key, position);
                !indexed) {
                return Status{make_unexpected(indexed.error())};
            }
        }
        holes_ = 0;
        return ok();
    }

    [[nodiscard]] usize size() const noexcept { return index_.size(); }
    [[nodiscard]] bool empty() const noexcept { return index_.empty(); }
    [[nodiscard]] usize hole_count() const noexcept { return holes_; }

    /// Iteration in insertion order, skipping removed entries.
    class Iterator {
    public:
        Iterator(const Array<Entry>* entries, usize index) noexcept
            : entries_(entries), index_(index) {
            skip();
        }

        [[nodiscard]] const Entry& operator*() const noexcept { return (*entries_)[index_]; }
        [[nodiscard]] const Entry* operator->() const noexcept { return &(*entries_)[index_]; }
        Iterator& operator++() noexcept {
            ++index_;
            skip();
            return *this;
        }
        [[nodiscard]] bool operator!=(const Iterator& other) const noexcept {
            return index_ != other.index_;
        }

    private:
        void skip() noexcept {
            while (index_ < entries_->size() && !(*entries_)[index_].live) {
                ++index_;
            }
        }
        const Array<Entry>* entries_;
        usize index_;
    };

    [[nodiscard]] Iterator begin() const noexcept { return Iterator(&entries_, 0); }
    [[nodiscard]] Iterator end() const noexcept { return Iterator(&entries_, entries_.size()); }

private:
    Array<Entry> entries_;
    HashMap<K, usize, Hasher, Equal> index_;
    usize holes_ = 0;
};

}  // namespace cy
