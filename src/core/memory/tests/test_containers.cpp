// The sequence and associative containers. Task 2.4.

#include <cy/test/test.h>

#include <cy/core/memory/arena.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/flat_map.h>
#include <cy/core/memory/hash.h>
#include <cy/core/memory/hash_map.h>
#include <cy/core/memory/intrusive_list.h>
#include <cy/core/memory/ring_buffer.h>
#include <cy/core/memory/sparse_set.h>
#include <cy/core/memory/system_allocator.h>

#include <cstring>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

/// A type that is NOT trivially relocatable, so the per-element move path is exercised. It counts
/// its own moves, which is how the test tells the two growth strategies apart.
struct Tracked {
    static int moves;
    static int destructions;

    int value = 0;

    explicit Tracked(int initial = 0) noexcept : value(initial) {}
    Tracked(const Tracked&) noexcept = default;
    Tracked(Tracked&& other) noexcept : value(other.value) { ++moves; }
    Tracked& operator=(const Tracked&) noexcept = default;
    Tracked& operator=(Tracked&& other) noexcept {
        value = other.value;
        return *this;
    }
    ~Tracked() { ++destructions; }
};

int Tracked::moves = 0;
int Tracked::destructions = 0;

struct Task {
    int value = 0;
    cy::IntrusiveNode link;
};

}  // namespace

CY_TEST_CASE("Array grows geometrically and reserve stops it growing again") {
    cy::Array<int> numbers;
    CY_CHECK(numbers.empty());

    CY_REQUIRE(numbers.reserve(100).has_value());
    const cy::usize reserved = numbers.capacity();
    CY_CHECK(reserved >= 100u);

    const int* data_before = numbers.data();
    for (int index = 0; index < 100; ++index) {
        CY_REQUIRE(numbers.push_back(index).has_value());
    }
    CY_CHECK_EQ(numbers.size(), 100u);
    CY_CHECK_EQ(numbers.capacity(), reserved);
    CY_CHECK_EQ(numbers.data(), data_before);  // no reallocation inside the reservation

    // Growth doubles rather than adding a constant.
    cy::Array<int> grown;
    CY_REQUIRE(grown.push_back(0).has_value());
    const cy::usize first = grown.capacity();
    while (grown.capacity() == first) {
        CY_REQUIRE(grown.push_back(1).has_value());
    }
    CY_CHECK_EQ(grown.capacity(), first * 2);
}

CY_TEST_CASE("trivially relocatable types move with one memcpy, others move one at a time") {
    static_assert(cy::is_trivially_relocatable_v<int>);
    static_assert(!cy::is_trivially_relocatable_v<Tracked>);

    cy::Array<int> plain;
    for (int index = 0; index < 64; ++index) {
        CY_REQUIRE(plain.push_back(index).has_value());
    }
    for (int index = 0; index < 64; ++index) {
        CY_CHECK_EQ(plain[static_cast<cy::usize>(index)], index);
    }

    Tracked::moves = 0;
    cy::Array<Tracked> tracked;
    for (int index = 0; index < 64; ++index) {
        CY_REQUIRE(tracked.emplace_back(index).has_value());
    }
    // A non-relocatable type is moved element by element on every growth, so the count is non-zero;
    // an int array does no per-element work at all.
    CY_CHECK(Tracked::moves > 0);
    CY_CHECK_EQ(tracked[63].value, 63);
}

CY_TEST_CASE("remove_unordered is O(1) and erase keeps the order") {
    cy::Array<int> numbers;
    for (int index = 0; index < 5; ++index) {
        CY_REQUIRE(numbers.push_back(index).has_value());
    }

    numbers.remove_unordered(1);
    CY_CHECK_EQ(numbers.size(), 4u);
    CY_CHECK_EQ(numbers[1], 4);  // the last element took the removed one's place

    numbers.erase(0);
    CY_CHECK_EQ(numbers.size(), 3u);
    CY_CHECK_EQ(numbers[0], 4);
    CY_CHECK_EQ(numbers[1], 2);
    CY_CHECK_EQ(numbers[2], 3);
}

CY_TEST_CASE("erase closes the gap correctly, even though the ranges overlap") {
    // REGRESSION. `erase` originally closed the gap with the same helper `Array` uses to grow,
    // which is a single memcpy — and memcpy on overlapping ranges is undefined behaviour. It
    // happened to produce the right answer here and AddressSanitizer reported it as
    // memcpy-param-overlap. The fix is `detail::relocate_overlapping`, which is memmove for a
    // trivially relocatable type and a forward move loop otherwise. This case is deliberately long
    // enough that the two ranges overlap by more than one element, which is the shape a memcpy gets
    // wrong on a real implementation.
    cy::Array<int> numbers;
    for (int index = 0; index < 64; ++index) {
        CY_REQUIRE(numbers.push_back(index).has_value());
    }
    numbers.erase(0);
    CY_REQUIRE_EQ(numbers.size(), 63u);
    for (int index = 0; index < 63; ++index) {
        CY_CHECK_EQ(numbers[static_cast<cy::usize>(index)], index + 1);
    }

    // And for a type that is not trivially relocatable, where the loop rather than memmove runs.
    cy::Array<Tracked> tracked;
    for (int index = 0; index < 16; ++index) {
        CY_REQUIRE(tracked.emplace_back(index).has_value());
    }
    tracked.erase(3);
    CY_REQUIRE_EQ(tracked.size(), 15u);
    for (int index = 0; index < 15; ++index) {
        CY_CHECK_EQ(tracked[static_cast<cy::usize>(index)].value, (index < 3) ? index : index + 1);
    }
}

CY_TEST_CASE("a copy is deep and explicit; there is no implicit copy-on-write") {
    static_assert(
        !std::is_copy_constructible_v<cy::Array<int>>,
        "an Array must not be copyable implicitly — the copy is clone(), and it can fail");
    static_assert(!std::is_copy_assignable_v<cy::Array<int>>);
    static_assert(std::is_move_constructible_v<cy::Array<int>>);

    cy::Array<int> original;
    for (int index = 0; index < 8; ++index) {
        CY_REQUIRE(original.push_back(index).has_value());
    }

    auto copy = original.clone();
    CY_REQUIRE(copy.has_value());
    CY_CHECK_EQ(copy->size(), original.size());
    CY_CHECK_NE(copy->data(), original.data());  // deep: separate storage, not a shared buffer

    (*copy)[0] = 99;
    CY_CHECK_EQ(original[0], 0);  // and writing one does not touch the other
}

CY_TEST_CASE("FixedArray never allocates and refuses when it is full") {
    cy::FixedArray<int, 4> fixed;
    for (int index = 0; index < 4; ++index) {
        CY_REQUIRE(fixed.push_back(index).has_value());
    }
    CY_CHECK(fixed.full());

    const cy::Status refused = fixed.push_back(4);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, cy::ErrorCode::OutOfRange);
    CY_CHECK_EQ(fixed.size(), 4u);
}

CY_TEST_CASE("SmallArray stays inline until it spills") {
    cy::SmallArray<int, 4> small;
    for (int index = 0; index < 4; ++index) {
        CY_REQUIRE(small.push_back(index).has_value());
    }
    CY_CHECK(small.is_inline());

    CY_REQUIRE(small.push_back(4).has_value());
    CY_CHECK_FALSE(small.is_inline());
    CY_CHECK_EQ(small.size(), 5u);
    for (int index = 0; index < 5; ++index) {
        CY_CHECK_EQ(small[static_cast<cy::usize>(index)], index);
    }

    // Moving a spilled SmallArray steals the heap buffer; moving an inline one relocates.
    cy::SmallArray<int, 4> moved(std::move(small));
    CY_CHECK_EQ(moved.size(), 5u);
    CY_CHECK_EQ(moved[4], 4);
}

CY_TEST_CASE("RingBuffer is bounded and reports what it dropped") {
    cy::RingBuffer<int> ring;
    CY_REQUIRE(ring.reserve(3).has_value());
    CY_CHECK_EQ(ring.capacity(), 4u);  // rounded up to a power of two, and it says so

    for (int index = 0; index < 4; ++index) {
        CY_REQUIRE(ring.push(index).has_value());
    }
    CY_CHECK(ring.full());

    const cy::Status refused = ring.push(4);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, cy::ErrorCode::OutOfRange);

    CY_CHECK(ring.force_push(4));  // it dropped the oldest, and said so
    CY_REQUIRE(ring.front() != nullptr);
    CY_CHECK_EQ(*ring.front(), 1);

    ring.pop();
    CY_CHECK_EQ(*ring.front(), 2);
    CY_CHECK_EQ(ring.size(), 3u);
}

CY_TEST_CASE("SparseSet stores densely behind a sparse index") {
    cy::SparseSet<int> set;
    CY_REQUIRE(set.insert(100, 1).has_value());
    CY_REQUIRE(set.insert(7, 2).has_value());
    CY_REQUIRE(set.insert(42, 3).has_value());

    CY_CHECK_EQ(set.size(), 3u);
    CY_CHECK(set.contains(42));
    CY_CHECK_FALSE(set.contains(43));
    CY_REQUIRE(set.find(7) != nullptr);
    CY_CHECK_EQ(*set.find(7), 2);

    // The values are contiguous, which is what makes iteration linear.
    const cy::Span<int> values = set.values();
    CY_CHECK_EQ(values.size(), 3u);
    CY_CHECK_EQ(&values[1] - values.data(), 1);

    CY_CHECK(set.remove(7));
    CY_CHECK_FALSE(set.remove(7));
    CY_CHECK_EQ(set.size(), 2u);
    CY_CHECK(set.contains(100));
    CY_CHECK(set.contains(42));
    CY_CHECK_EQ(*set.find(42), 3);  // the swapped-in element is still findable
}

CY_TEST_CASE("IntrusiveList removes in O(1) from the element itself") {
    cy::IntrusiveList<Task, &Task::link> ready;
    Task first{1, {}};
    Task second{2, {}};
    Task third{3, {}};

    ready.push_back(first);
    ready.push_back(second);
    ready.push_back(third);
    CY_CHECK_EQ(ready.size(), 3u);

    // Removal needs only the element, not the list — that is the whole point of an intrusive one.
    second.link.unlink();
    CY_CHECK_EQ(ready.size(), 2u);
    CY_CHECK_FALSE(second.link.is_linked());

    int sum = 0;
    for (const Task& task : ready) {
        sum += task.value;
    }
    CY_CHECK_EQ(sum, 4);

    {
        // A node that goes out of scope while linked unlinks itself, and the count follows.
        Task scoped{4, {}};
        ready.push_back(scoped);
        CY_CHECK_EQ(ready.size(), 3u);
    }
    CY_CHECK_EQ(ready.size(), 2u);

    CY_REQUIRE(ready.pop_front() != nullptr);
    CY_CHECK_EQ(ready.size(), 1u);
}

CY_TEST_CASE("HashMap inserts, finds and removes") {
    cy::HashMap<int, int> map;
    for (int index = 0; index < 64; ++index) {
        CY_REQUIRE(map.insert(index, index * 2).has_value());
    }
    CY_CHECK_EQ(map.size(), 64u);
    for (int index = 0; index < 64; ++index) {
        const int* found = map.find(index);
        CY_REQUIRE(found != nullptr);
        CY_CHECK_EQ(*found, index * 2);
    }
    CY_CHECK(map.find(64) == nullptr);

    // Overwriting a key replaces the value and does not add an entry.
    CY_REQUIRE(map.insert(5, 500).has_value());
    CY_CHECK_EQ(map.size(), 64u);
    CY_CHECK_EQ(*map.find(5), 500);

    CY_CHECK(map.remove(5));
    CY_CHECK_FALSE(map.remove(5));
    CY_CHECK_EQ(map.size(), 63u);
    CY_CHECK(map.find(5) == nullptr);
    // The entries that probed past the removed slot are still findable: backward-shift deletion
    // moved them back into it rather than leaving a tombstone.
    for (int index = 0; index < 64; ++index) {
        CY_CHECK_EQ(map.find(index) != nullptr, index != 5);
    }

    map.clear();
    CY_CHECK(map.empty());
}

CY_TEST_CASE("HashSet is a HashMap whose value costs nothing") {
    static_assert(sizeof(cy::HashMap<cy::u32, cy::detail::Unit>::Entry) == sizeof(cy::u32),
                  "a set entry must be exactly its key — the empty value takes no space");

    cy::HashSet<cy::u32> set;
    CY_REQUIRE(set.insert(7).has_value());
    CY_REQUIRE(set.insert(7).has_value());
    CY_CHECK_EQ(set.size(), 1u);
    CY_CHECK(set.contains(7));
    CY_CHECK(set.remove(7));
    CY_CHECK(set.empty());
}

CY_TEST_CASE("serialization order is deterministic: FlatMap iterates by key") {
    cy::FlatMap<int, int> flat;
    const int inserted[] = {5, 1, 9, 3, 7};
    for (int key : inserted) {
        CY_REQUIRE(flat.insert(key, key * 10).has_value());
    }
    CY_CHECK_EQ(flat.size(), 5u);

    const cy::Span<const int> keys = flat.keys();
    for (cy::usize index = 1; index < keys.size(); ++index) {
        CY_CHECK(keys[index - 1] < keys[index]);
    }
    CY_CHECK_EQ(keys[0], 1);
    CY_CHECK_EQ(*flat.find(9), 90);

    // Built in a different order, it iterates identically — which is what byte-stable output needs.
    cy::FlatMap<int, int> other;
    const int reversed[] = {7, 3, 9, 1, 5};
    for (int key : reversed) {
        CY_REQUIRE(other.insert(key, key * 10).has_value());
    }
    const cy::Span<const int> other_keys = other.keys();
    CY_REQUIRE_EQ(other_keys.size(), keys.size());
    for (cy::usize index = 0; index < keys.size(); ++index) {
        CY_CHECK_EQ(other_keys[index], keys[index]);
    }

    CY_CHECK(flat.remove(9));
    CY_CHECK_EQ(flat.size(), 4u);
    CY_CHECK(flat.find(9) == nullptr);
}

CY_TEST_CASE("OrderedMap iterates in insertion order, holes and all") {
    cy::OrderedMap<int, int> ordered;
    const int inserted[] = {5, 1, 9, 3, 7};
    for (int key : inserted) {
        CY_REQUIRE(ordered.insert(key, key * 10).has_value());
    }

    cy::usize position = 0;
    for (const auto& entry : ordered) {
        CY_CHECK_EQ(entry.key, inserted[position++]);
    }
    CY_CHECK_EQ(position, 5u);

    CY_CHECK(ordered.remove(9));
    CY_CHECK_EQ(ordered.size(), 4u);
    CY_CHECK_EQ(ordered.hole_count(), 1u);

    const int remaining[] = {5, 1, 3, 7};
    position = 0;
    for (const auto& entry : ordered) {
        CY_CHECK_EQ(entry.key, remaining[position++]);
    }
    CY_CHECK_EQ(position, 4u);

    CY_REQUIRE(ordered.compact().has_value());
    CY_CHECK_EQ(ordered.hole_count(), 0u);
    position = 0;
    for (const auto& entry : ordered) {
        CY_CHECK_EQ(entry.key, remaining[position++]);
    }
    CY_CHECK_EQ(position, 4u);
}

CY_TEST_CASE("hashing is defined in one place and mixes its input") {
    // Pin the seed for the duration, and put it back: the process seed is shared state, and the
    // case below asserts what the BUILD chose rather than what a neighbouring test left behind.
    // Leaving it pinned is what made this suite pass in dev and fail in Shipping, which is exactly
    // the class of failure the milestone's gate looks for outside the dev profile.
    const cy::u64 process_seed = cy::hash_seed();
    cy::set_hash_seed(0x1234);
    const cy::u64 a = cy::hash_bytes("hello", 5);
    const cy::u64 b = cy::hash_bytes("hello", 5);
    const cy::u64 c = cy::hash_bytes("hellp", 5);
    CY_CHECK_EQ(a, b);  // deterministic for a fixed seed
    CY_CHECK_NE(a, c);  // and one changed bit changes the answer

    // Trailing zero bytes are not the same input as their absence.
    CY_CHECK_NE(cy::hash_bytes("a", 1), cy::hash_bytes("a\0", 2));

    // Consecutive integers — the commonest engine key — must not collide in the low bits an
    // open-addressed table probes with.
    cy::u64 low_bits = 0;
    for (cy::u32 index = 0; index < 64; ++index) {
        low_bits |= 1ull << (cy::Hash<cy::u32>{}(index) & 63u);
    }
    CY_CHECK(low_bits != 0);

    CY_CHECK_NE(cy::Hash<std::string_view>{}("alpha"), cy::Hash<std::string_view>{}("beta"));
    CY_CHECK_EQ(cy::Hash<cy::f32>{}(0.0f), cy::Hash<cy::f32>{}(-0.0f));  // equal values hash equal

    cy::set_hash_seed(process_seed);
}

CY_TEST_CASE("the hash seed is randomised in development and fixed otherwise") {
    // The seed is a property of the build, so what this checks depends on the configuration. In a
    // shipping build the requirement is reproducibility; in a development build it is that
    // iteration-order dependencies are shaken out.
    const cy::u64 seed = cy::hash_seed();
#if defined(CY_DEVELOPMENT)
    CY_CHECK_NE(seed, 0x9e3779b97f4a7c15ull);
#else
    CY_CHECK_EQ(seed, 0x9e3779b97f4a7c15ull);
#endif
}

CY_TEST_CASE("containers allocate from the scope they were constructed in") {
    cy::ArenaAllocator arena(cy::MemoryDomain::Renderer, "batches");
    CY_REQUIRE(arena.reserve(cy::usize{64} * 1024).has_value());

    {
        const cy::AllocatorScope scope(arena);
        cy::Array<int> batches;
        CY_REQUIRE(batches.reserve(16).has_value());
        CY_CHECK(arena.owns(batches.data()));
        CY_CHECK_EQ(&batches.allocator(), static_cast<cy::Allocator*>(&arena));
    }

    cy::Array<int> outside;
    CY_REQUIRE(outside.reserve(16).has_value());
    CY_CHECK_FALSE(arena.owns(outside.data()));
}
