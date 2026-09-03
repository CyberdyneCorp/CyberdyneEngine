#pragma once
// Open addressing with robin-hood probing. Task 2.4.
//
// `core-memory-and-containers` — "Associative containers": `HashMap<K, V>` and `HashSet<K>` use
// open addressing with robin-hood probing. Hashing is defined in one place (`hash.h`), seeded per
// process in development builds, and deterministic in shipping builds.
//
// ROBIN HOOD, IN ONE PARAGRAPH. Every occupied slot knows how far it is from the slot its hash
// wanted — its probe distance. On insertion, when the entry being placed has travelled further than
// the entry already sitting in a slot, the two swap and the poorer one carries on looking. The
// effect is that probe distances stay close to their mean instead of forming long tails, so the
// worst lookup is close to the average one, which is the property a frame budget cares about.
//
// ITERATION ORDER IS NOT DEFINED, ON PURPOSE. It depends on the seed, and the seed is randomised in
// development builds so that code depending on the order fails in testing rather than in a
// serialized artefact. Anything that must iterate in a defined order uses `OrderedMap` or
// `FlatMap` (`flat_map.h`).

#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/hash.h>
#include <cy/core/memory/relocatable.h>
#include <cy/core/memory/scope.h>

#include <functional>
#include <utility>

namespace cy {

namespace detail {

/// The value half of a `HashSet` entry. Empty, and marked so that it occupies no space in the
/// entry — which is what makes a set of `u32` cost four bytes per slot rather than eight.
struct Unit {};

/// The load factor at which the table doubles. 0.85 is high for open addressing generally and
/// affordable with robin hood, because the probe distances stay bounded rather than degrading.
inline constexpr u64 kHashMapLoadPercent = 85;

}  // namespace detail

/// An open-addressed map. Move-only, like every owning container here; `clone()` is the deep copy.
template <class K, class V, class Hasher = Hash<K>, class Equal = std::equal_to<K>>
class HashMap {
public:
    struct Entry {
        K key;
        // Empty for a HashSet, and then costing nothing.
        [[no_unique_address]] V value;
    };

    explicit HashMap(Allocator& allocator = current_allocator()) noexcept
        : allocator_(&allocator) {}

    ~HashMap() { release(); }

    HashMap(const HashMap&) = delete;
    HashMap& operator=(const HashMap&) = delete;

    HashMap(HashMap&& other) noexcept
        : allocator_(other.allocator_),
          hashes_(other.hashes_),
          entries_(other.entries_),
          mask_(other.mask_),
          size_(other.size_),
          capacity_(other.capacity_) {
        other.hashes_ = nullptr;
        other.entries_ = nullptr;
        other.mask_ = 0;
        other.size_ = 0;
        other.capacity_ = 0;
    }

    HashMap& operator=(HashMap&& other) noexcept {
        if (this != &other) {
            release();
            allocator_ = other.allocator_;
            hashes_ = other.hashes_;
            entries_ = other.entries_;
            mask_ = other.mask_;
            size_ = other.size_;
            capacity_ = other.capacity_;
            other.hashes_ = nullptr;
            other.entries_ = nullptr;
            other.mask_ = 0;
            other.size_ = 0;
            other.capacity_ = 0;
        }
        return *this;
    }

    /// Make room for `wanted` entries without further growth.
    [[nodiscard]] Status reserve(usize wanted) noexcept {
        const usize needed =
            (wanted * 100 + detail::kHashMapLoadPercent - 1) / detail::kHashMapLoadPercent;
        if (needed <= capacity_) {
            return ok();
        }
        usize target = (capacity_ == 0) ? 16 : capacity_;
        while (target < needed) {
            target *= 2;
        }
        return rehash(target);
    }

    /// Insert, or overwrite the value of an existing key. Returns the stored value.
    [[nodiscard]] Expected<V*, Error> insert(const K& key, V value) noexcept {
        if (Status room = ensure_room(); !room) {
            return make_unexpected(room.error());
        }
        return place(key, std::move(value));
    }

    [[nodiscard]] V* find(const K& key) noexcept {
        const usize slot = locate(key);
        return (slot == kNoSlot) ? nullptr : &entries_[slot].value;
    }
    [[nodiscard]] const V* find(const K& key) const noexcept {
        const usize slot = locate(key);
        return (slot == kNoSlot) ? nullptr : &entries_[slot].value;
    }

    [[nodiscard]] bool contains(const K& key) const noexcept { return locate(key) != kNoSlot; }

    /// Remove `key`. Backward-shift deletion rather than a tombstone: the entries that probed past
    /// this slot are moved back into it, so a table that is filled and emptied repeatedly does not
    /// accumulate tombstones and degrade into a linear scan.
    bool remove(const K& key) noexcept {
        usize slot = locate(key);
        if (slot == kNoSlot) {
            return false;
        }
        entries_[slot].~Entry();
        hashes_[slot] = 0;
        --size_;

        usize next = (slot + 1) & mask_;
        while (hashes_[next] != 0 && probe_distance(next) != 0) {
            hashes_[slot] = hashes_[next];
            construct_at<Entry>(&entries_[slot], std::move(entries_[next]));
            entries_[next].~Entry();
            hashes_[next] = 0;
            slot = next;
            next = (next + 1) & mask_;
        }
        return true;
    }

    void clear() noexcept {
        for (usize slot = 0; slot < capacity_; ++slot) {
            if (hashes_[slot] != 0) {
                entries_[slot].~Entry();
                hashes_[slot] = 0;
            }
        }
        size_ = 0;
    }

    [[nodiscard]] usize size() const noexcept { return size_; }
    [[nodiscard]] usize capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    /// The longest probe any live entry is from its ideal slot. A table whose worst probe is
    /// growing has a hash that is not distributing, and that is a number a report can watch.
    [[nodiscard]] u32 worst_probe_distance() const noexcept {
        u32 worst = 0;
        for (usize slot = 0; slot < capacity_; ++slot) {
            if (hashes_[slot] == 0) {
                continue;
            }
            const auto distance = static_cast<u32>(probe_distance(slot));
            worst = (distance > worst) ? distance : worst;
        }
        return worst;
    }

    /// Iteration over occupied slots, in table order — which is to say, in no order the caller may
    /// rely on. See the note at the top of this file.
    class Iterator {
    public:
        Iterator(const HashMap* map, usize slot) noexcept : map_(map), slot_(slot) { skip(); }

        [[nodiscard]] const Entry& operator*() const noexcept { return map_->entries_[slot_]; }
        [[nodiscard]] const Entry* operator->() const noexcept { return &map_->entries_[slot_]; }
        Iterator& operator++() noexcept {
            ++slot_;
            skip();
            return *this;
        }
        [[nodiscard]] bool operator!=(const Iterator& other) const noexcept {
            return slot_ != other.slot_;
        }

    private:
        void skip() noexcept {
            while (slot_ < map_->capacity_ && map_->hashes_[slot_] == 0) {
                ++slot_;
            }
        }
        const HashMap* map_;
        usize slot_;
    };

    [[nodiscard]] Iterator begin() const noexcept { return Iterator(this, 0); }
    [[nodiscard]] Iterator end() const noexcept { return Iterator(this, capacity_); }

private:
    static constexpr usize kNoSlot = static_cast<usize>(-1);

    /// A hash of zero would be indistinguishable from an empty slot, so it is moved to one. The
    /// distribution loss is one value in 2^64.
    [[nodiscard]] static u64 hash_of(const K& key) noexcept {
        const u64 value = Hasher{}(key);
        return (value == 0) ? 1 : value;
    }

    [[nodiscard]] usize probe_distance(usize slot) const noexcept {
        const usize ideal = static_cast<usize>(hashes_[slot]) & mask_;
        return (slot + capacity_ - ideal) & mask_;
    }

    [[nodiscard]] usize locate(const K& key) const noexcept {
        if (size_ == 0) {
            return kNoSlot;
        }
        const u64 hash = hash_of(key);
        usize slot = static_cast<usize>(hash) & mask_;
        usize distance = 0;
        while (hashes_[slot] != 0) {
            // Robin hood's other half: entries are ordered by probe distance, so an entry poorer
            // than the one being looked for proves the key is absent without scanning further.
            if (distance > probe_distance(slot)) {
                return kNoSlot;
            }
            if (hashes_[slot] == hash && Equal{}(entries_[slot].key, key)) {
                return slot;
            }
            slot = (slot + 1) & mask_;
            ++distance;
        }
        return kNoSlot;
    }

    [[nodiscard]] Status ensure_room() noexcept {
        if (capacity_ != 0 && (size_ + 1) * 100 <= capacity_ * detail::kHashMapLoadPercent) {
            return ok();
        }
        return rehash((capacity_ == 0) ? 16 : capacity_ * 2);
    }

    /// Insert into a table that is known to have room. Returns the value's address.
    [[nodiscard]] V* place(const K& key, V&& value) noexcept {
        const u64 hash = hash_of(key);
        usize slot = static_cast<usize>(hash) & mask_;
        usize distance = 0;
        Entry carried{key, std::move(value)};
        u64 carried_hash = hash;
        V* result = nullptr;

        while (true) {
            if (hashes_[slot] == 0) {
                hashes_[slot] = carried_hash;
                Entry* placed = construct_at<Entry>(&entries_[slot], std::move(carried));
                ++size_;
                return (result != nullptr) ? result : &placed->value;
            }
            if (hashes_[slot] == carried_hash && Equal{}(entries_[slot].key, carried.key)) {
                // Overwriting an existing key. Only possible on the first iteration: after a swap
                // the carried entry is one the table already held, and it is unique.
                entries_[slot].value = std::move(carried.value);
                return &entries_[slot].value;
            }
            const usize occupant_distance = probe_distance(slot);
            if (occupant_distance < distance) {
                // The occupant is richer than the entry being carried, so they change places and
                // the poorer one — now the occupant — keeps looking.
                std::swap(hashes_[slot], carried_hash);
                std::swap(entries_[slot], carried);
                if (result == nullptr) {
                    result = &entries_[slot].value;
                }
                distance = occupant_distance;
            }
            slot = (slot + 1) & mask_;
            ++distance;
        }
    }

    [[nodiscard]] Status rehash(usize new_capacity) noexcept {
        void* hash_block = allocator_->allocate(new_capacity * sizeof(u64), alignof(u64));
        if (hash_block == nullptr) {
            return fail(ErrorCode::OutOfMemory, "HashMap growth was refused by its allocator");
        }
        void* entry_block = allocator_->allocate(new_capacity * sizeof(Entry), alignof(Entry));
        if (entry_block == nullptr) {
            allocator_->deallocate(hash_block, new_capacity * sizeof(u64), alignof(u64));
            return fail(ErrorCode::OutOfMemory, "HashMap growth was refused by its allocator");
        }

        u64* old_hashes = hashes_;
        Entry* old_entries = entries_;
        const usize old_capacity = capacity_;

        hashes_ = static_cast<u64*>(hash_block);
        entries_ = static_cast<Entry*>(entry_block);
        capacity_ = new_capacity;
        mask_ = new_capacity - 1;
        size_ = 0;
        for (usize slot = 0; slot < new_capacity; ++slot) {
            hashes_[slot] = 0;
        }

        for (usize slot = 0; slot < old_capacity; ++slot) {
            if (old_hashes[slot] == 0) {
                continue;
            }
            (void)place(old_entries[slot].key, std::move(old_entries[slot].value));
            old_entries[slot].~Entry();
        }
        if (old_hashes != nullptr) {
            allocator_->deallocate(old_hashes, old_capacity * sizeof(u64), alignof(u64));
            allocator_->deallocate(old_entries, old_capacity * sizeof(Entry), alignof(Entry));
        }
        return ok();
    }

    void release() noexcept {
        if (hashes_ == nullptr) {
            return;
        }
        clear();
        allocator_->deallocate(hashes_, capacity_ * sizeof(u64), alignof(u64));
        allocator_->deallocate(entries_, capacity_ * sizeof(Entry), alignof(Entry));
        hashes_ = nullptr;
        entries_ = nullptr;
        capacity_ = 0;
        mask_ = 0;
        size_ = 0;
    }

    Allocator* allocator_;
    u64* hashes_ = nullptr;  // 0 means empty; every other value is a live entry's hash
    Entry* entries_ = nullptr;
    usize mask_ = 0;
    usize size_ = 0;
    usize capacity_ = 0;
};

/// A set is a map whose value occupies no space. One implementation, not two.
template <class K, class Hasher = Hash<K>, class Equal = std::equal_to<K>>
class HashSet {
public:
    explicit HashSet(Allocator& allocator = current_allocator()) noexcept : map_(allocator) {}

    [[nodiscard]] Status reserve(usize wanted) noexcept { return map_.reserve(wanted); }

    [[nodiscard]] Status insert(const K& key) noexcept {
        Expected<detail::Unit*, Error> slot = map_.insert(key, detail::Unit{});
        return slot ? ok() : Status{make_unexpected(slot.error())};
    }

    [[nodiscard]] bool contains(const K& key) const noexcept { return map_.contains(key); }
    bool remove(const K& key) noexcept { return map_.remove(key); }
    void clear() noexcept { map_.clear(); }
    [[nodiscard]] usize size() const noexcept { return map_.size(); }
    [[nodiscard]] bool empty() const noexcept { return map_.empty(); }

    [[nodiscard]] auto begin() const noexcept { return map_.begin(); }
    [[nodiscard]] auto end() const noexcept { return map_.end(); }

private:
    HashMap<K, detail::Unit, Hasher, Equal> map_;
};

}  // namespace cy
