// The container properties that only show up over many operations. Task 2.4.
//
// An integration test rather than a unit one, and for the reason the taxonomy gives: cost. Filling
// and emptying a table twenty times over is the only way to demonstrate that backward-shift
// deletion does not degrade, and it does not fit a unit test's millisecond in the Debug
// configuration — which is where the budget guard caught it, not in dev.

#include <cy/test/test.h>

#include <cy/core/memory/array.h>
#include <cy/core/memory/flat_map.h>
#include <cy/core/memory/hash_map.h>
#include <cy/core/memory/sparse_set.h>

CY_TEST_CASE("filling and emptying a HashMap repeatedly leaves no tombstones behind") {
    cy::HashMap<int, int> map;
    constexpr int kEntries = 2000;

    for (int index = 0; index < kEntries; ++index) {
        CY_REQUIRE(map.insert(index, index).has_value());
    }
    const cy::usize settled_capacity = map.capacity();
    const cy::u32 first_worst = map.worst_probe_distance();

    for (int round = 0; round < 20; ++round) {
        for (int index = 0; index < kEntries; ++index) {
            CY_REQUIRE(map.remove(index));
        }
        CY_REQUIRE(map.empty());
        for (int index = 0; index < kEntries; ++index) {
            CY_REQUIRE(map.insert(index, index).has_value());
        }
    }

    CY_CHECK_EQ(map.size(), static_cast<cy::usize>(kEntries));
    // A tombstone implementation would have grown the table and lengthened every probe by now.
    CY_CHECK_EQ(map.capacity(), settled_capacity);
    CY_CHECK(map.worst_probe_distance() <= first_worst + 4u);
    // Robin hood's guarantee is that the worst probe stays close to the mean, which for a table at
    // 85% is a small constant rather than a function of the entry count.
    CY_CHECK(map.worst_probe_distance() < 40u);

    for (int index = 0; index < kEntries; ++index) {
        CY_REQUIRE(map.find(index) != nullptr);
    }
}

CY_TEST_CASE("an Array survives many growths with its contents intact") {
    cy::Array<cy::u64> numbers;
    constexpr cy::u64 kCount = 200000;
    for (cy::u64 index = 0; index < kCount; ++index) {
        CY_REQUIRE(numbers.push_back(index * 3).has_value());
    }
    CY_CHECK_EQ(numbers.size(), kCount);

    cy::u64 checksum = 0;
    for (cy::u64 value : numbers) {
        checksum += value;
    }
    CY_CHECK_EQ(checksum, kCount * (kCount - 1) / 2 * 3);

    // Geometric growth means the number of reallocations is logarithmic, so the capacity is within
    // a factor of two of the size rather than the size plus a constant.
    CY_CHECK(numbers.capacity() < kCount * 2);

    CY_REQUIRE(numbers.shrink_to_fit().has_value());
    CY_CHECK_EQ(numbers.capacity(), kCount);
    CY_CHECK_EQ(numbers[kCount - 1], (kCount - 1) * 3);
}

CY_TEST_CASE("a SparseSet stays correct through many insertions and removals") {
    cy::SparseSet<cy::u64> set;
    constexpr cy::u32 kKeys = 20000;

    for (cy::u32 key = 0; key < kKeys; ++key) {
        CY_REQUIRE(set.insert(key, key).has_value());
    }
    CY_CHECK_EQ(set.size(), kKeys);

    // Remove every other key: each removal swaps the last element into the gap, so the sparse index
    // of the moved element has to be fixed up, and getting that wrong is silent.
    for (cy::u32 key = 0; key < kKeys; key += 2) {
        CY_REQUIRE(set.remove(key));
    }
    CY_CHECK_EQ(set.size(), kKeys / 2);
    for (cy::u32 key = 0; key < kKeys; ++key) {
        const cy::u64* found = set.find(key);
        if ((key % 2) == 0) {
            CY_REQUIRE(found == nullptr);
        } else {
            CY_REQUIRE(found != nullptr);
            CY_CHECK_EQ(*found, key);
        }
    }
}

CY_TEST_CASE("a FlatMap keeps its keys sorted through many insertions") {
    cy::FlatMap<cy::u32, cy::u32> flat;
    constexpr cy::u32 kEntries = 4000;

    // Inserted in an order that is neither sorted nor reverse sorted, so the insertion point is in
    // the middle most of the time and the shift is the work being exercised.
    cy::u32 key = 1;
    for (cy::u32 index = 0; index < kEntries; ++index) {
        key = (key * 2654435761u) % 100003u;
        CY_REQUIRE(flat.insert(key, index).has_value());
    }

    const cy::Span<const cy::u32> keys = flat.keys();
    for (cy::usize index = 1; index < keys.size(); ++index) {
        CY_REQUIRE(keys[index - 1] < keys[index]);
    }
    for (cy::u32 sorted_key : keys) {
        CY_REQUIRE(flat.find(sorted_key) != nullptr);
    }
}
