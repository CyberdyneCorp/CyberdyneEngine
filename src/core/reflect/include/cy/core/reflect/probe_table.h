// The one open-addressed table behind every reflected lookup. Tasks 1.1 and 1.2 of M2.
//
// `core-type-system` states the complexity as part of the contract: "Type and field lookup SHALL
// NOT be linear in the number of registered types or fields." Two places need that — the registry's
// type index and FieldIndex's field index — and both want the same table, so the table is written
// once here rather than twice there.
//
// It is deliberately not a container. The caller owns the slot storage and decides how big it is,
// which is what lets the registry keep its slots in the same allocation discipline as its entries
// and lets FieldIndex size its table from a field count it already knows. `misc-definitions-in-
// headers` is satisfied because everything here is either constexpr or a template.
//
// WHY OPEN ADDRESSING AND NOT A SORTED ARRAY WITH A BINARY SEARCH. A binary search would need the
// generated FieldInfo array to be sorted by FieldId, and it is emitted in declaration order — a
// field added to the middle of a struct years later carries a higher identifier than the one after
// it. Sorting at registration would mean copying every descriptor out of the constexpr data it
// lives in, which is the one thing this module does not do.
//
// WHY FIBONACCI HASHING. A TypeId and a FieldId are counters the identity manifest hands out, so
// consecutive identifiers are the norm and the low bits are all the entropy there is. Multiplying
// by 2^32/phi and keeping the *high* bits spreads a run of counters across the table instead of
// packing it into one contiguous cluster, which is exactly the case linear probing handles worst.

#ifndef CY_CORE_REFLECT_PROBE_TABLE_H
#define CY_CORE_REFLECT_PROBE_TABLE_H

#include <cy/core/base/types.h>

namespace cy::reflect::detail {

/// How many slots a table holding `entries` keys needs: a power of two with room to spare.
///
/// Twice the entry count, so the load factor never exceeds one half. That is what bounds the probe
/// length to a small constant rather than to the table's size, and it is the whole reason the
/// lookup below is not linear in disguise. Eight is the floor because a table smaller than a cache
/// line buys nothing.
[[nodiscard]] constexpr u32 table_slots_for(u32 entries) noexcept {
    u32 slots = 8;
    while (slots < entries * 2 && slots < (1u << 31u)) {
        slots *= 2;
    }
    return slots;
}

/// 2^32 divided by the golden ratio. The multiplier of Knuth's multiplicative hash.
inline constexpr u32 golden_ratio_32 = 0x9E3779B9u;

/// FNV-1a over a NUL-terminated string. Names are metadata, so this is only ever on a diagnostic or
/// a tooling path; it is here so that the name table probes the same way the identifier table does.
[[nodiscard]] constexpr u32 hash_name(const char* text) noexcept {
    u32 hash = 0x811C9DC5u;
    if (text == nullptr) {
        return hash;
    }
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        hash ^= static_cast<u32>(static_cast<unsigned char>(*cursor));
        hash *= 0x01000193u;
    }
    return hash;
}

/// A table of u32 slots over storage the caller owns. Slot 0 is empty; any other slot holds an
/// entry index biased by one, so that "empty" needs no sentinel index and no separate occupancy
/// bitmap.
///
/// Insert-only. Nothing in this module removes a single type or a single field — a registry is
/// cleared wholesale and a FieldIndex is rebuilt — so there are no tombstones, and a probe that
/// reaches an empty slot has proved the key absent.
class ProbeTable {
public:
    ProbeTable() noexcept = default;

    /// Take `slot_count` slots of the caller's storage, which must be a power of two, and empty
    /// them. Copying a ProbeTable copies its pointer, which is correct only while the storage it
    /// points at outlives both — every owner here keeps the two together in one object.
    void adopt(u32* slots, u32 slot_count) noexcept {
        slots_ = slots;
        mask_ = slot_count - 1;
        // 32 - log2(slot_count), counted down from 31 rather than 32 so that the shift below is
        // never 32 even for a degenerate one-slot table, which would be undefined rather than
        // merely wrong. table_slots_for() never returns fewer than eight; this is what makes that
        // a nicety rather than the only thing standing between here and undefined behaviour.
        shift_ = 31;
        for (u32 size = slot_count; size > 2; size >>= 1u) {
            --shift_;
        }
        longest_probe_ = 0;
        for (u32 index = 0; index < slot_count; ++index) {
            slots_[index] = 0;
        }
    }

    [[nodiscard]] bool ready() const noexcept { return slots_ != nullptr; }

    /// The entry index for `hash`, or `absent`. `matches(candidate)` decides whether an occupied
    /// slot really holds the key, because a hash equal is not a key equal.
    template <class Match>
    [[nodiscard]] u32 find(u32 hash, Match matches) const noexcept {
        if (slots_ == nullptr) {
            return absent;
        }
        u32 slot = ((hash * golden_ratio_32) >> shift_) & mask_;
        for (u32 step = 0; step <= mask_; ++step) {
            const u32 occupant = slots_[slot];
            if (occupant == 0) {
                return absent;
            }
            if (matches(occupant - 1)) {
                return occupant - 1;
            }
            slot = (slot + 1) & mask_;
        }
        return absent;
    }

    /// Record `entry` under `hash`. False when `matches` found the key already present — which is
    /// how a duplicate identifier is caught, and is the caller's error to report.
    ///
    /// The table cannot fill: every owner sizes it through table_slots_for(), so at most half the
    /// slots are ever occupied and the loop always finds an empty one.
    template <class Match>
    bool insert(u32 hash, u32 entry, Match matches) noexcept {
        u32 slot = ((hash * golden_ratio_32) >> shift_) & mask_;
        for (u32 step = 0; step <= mask_; ++step) {
            if (slots_[slot] == 0) {
                slots_[slot] = entry + 1;
                longest_probe_ = (step + 1 > longest_probe_) ? step + 1 : longest_probe_;
                return true;
            }
            if (matches(slots_[slot] - 1)) {
                return false;
            }
            slot = (slot + 1) & mask_;
        }
        return false;
    }

    /// The longest chain any successful lookup can walk, measured as the table was filled.
    ///
    /// This is what makes "lookup does not degrade with the registry" a deterministic assertion
    /// rather than a stopwatch reading: a linear scan's worst chain grows with the entry count and
    /// this one does not, and a test can say so on any machine under any load.
    [[nodiscard]] u32 longest_probe() const noexcept { return longest_probe_; }

    static constexpr u32 absent = ~0u;

private:
    u32* slots_ = nullptr;
    u32 mask_ = 0;
    u32 shift_ = 31;  ///< 32 − log2(slot_count): how far down to shift for the high bits.
    u32 longest_probe_ = 0;
};

}  // namespace cy::reflect::detail

#endif  // CY_CORE_REFLECT_PROBE_TABLE_H
