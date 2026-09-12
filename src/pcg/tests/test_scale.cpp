// The two properties that are only true at scale: no per-point heap allocation, and provenance that
// is actually strippable. M10 tasks.md 4.1 and 4.3.
//
// `procedural-content-generation` — "PCG performance": "Generation SHALL support regions containing
// MILLIONS OF CANDIDATE POINTS WITHOUT PER-POINT HEAP ALLOCATION", and its scenario: "WHEN a region
// generates millions of candidates THEN no per-point heap allocation SHALL occur."
// And "Provenance": "Provenance SHALL be strippable from shipping builds."
//
// ================================================================================================
// WHY AN ALLOCATOR AND NOT A TIMER
// ================================================================================================
//
// "No per-point heap allocation" is a statement about allocations, so it is measured in
// allocations. A timing test would pass on a machine with a fast allocator and fail on a loaded
// one, and would never say which of the two it was. The counting allocator below forwards
// everything and remembers nothing about addresses — see src/replay/tests/test_teardown.cpp for why
// `cy::TrackingAllocator` is not usable for this.
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL, AND WHAT THAT EXPOSED ABOUT IT: the capacity guard
// in `PointSet::add()` was removed so the arrays would grow instead of refusing. "A million points
// after one reserve allocate nothing further" STAYED GREEN — `Array::reserve()` rounds up to a
// power of two, so a million adds into a 1 048 576-slot buffer never reach the growth path whether
// or not the guard is there — and it was `test_dataset.cpp`'s "a point set refuses to grow past the
// capacity it reserved" that went red, at a fifth point added to a set reserved for four. The guard
// was then restored.
//
// That is worth writing down rather than tidying away: THIS SUITE MEASURES THAT THE ALLOCATION DOES
// NOT HAPPEN AND THE OTHER ONE MEASURES THAT THE REFUSAL DOES, and only the pair of them covers the
// requirement. A suite that claimed the mutation on its own would have been claiming a red it never
// saw. Both runs are reported in this milestone's `verified_failing`.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/pcg/dataset.h>
#include <cy/pcg/execute.h>

#include "fixtures.h"

using cy::pcg::AttributeDecl;
using cy::pcg::AttributeId;
using cy::pcg::AttributeType;
using cy::pcg::ExecutionDomain;
using cy::pcg::FlatSpatialQuery;
using cy::pcg::GenerationContext;
using cy::pcg::GenerationWorld;
using cy::pcg::Generator;
using cy::pcg::PointSet;
using cy::pcg::ProvenanceMode;
using cy::pcg::RegionCoord;
namespace test = cy::pcg::test;

namespace {

/// Counts blocks and bytes outstanding. Forwards everything; remembers nothing about addresses, so
/// it cannot mistake an upstream's address reuse for a double free.
class CountingAllocator final : public cy::Allocator {
public:
    CountingAllocator() noexcept
        : cy::Allocator(cy::MemoryDomain::World, "pcg-scale"),
          upstream_(cy::system_allocator(cy::MemoryDomain::World)) {}

    [[nodiscard]] cy::u64 total_blocks() const noexcept { return total_blocks_; }
    [[nodiscard]] cy::u64 live_bytes() const noexcept { return live_bytes_; }

protected:
    [[nodiscard]] void* do_allocate(cy::usize size, cy::usize alignment) noexcept override {
        void* block = upstream_.allocate(size, alignment);
        if (block != nullptr) {
            ++total_blocks_;
            live_bytes_ += size;
        }
        return block;
    }

    [[nodiscard]] void* do_reallocate(void* pointer, cy::usize old_size, cy::usize new_size,
                                      cy::usize alignment) noexcept override {
        void* block = upstream_.reallocate(pointer, old_size, new_size, alignment);
        if (block != nullptr) {
            // A reallocation IS an allocation for this measurement: it is the growth a per-point
            // heap allocation shows up as, and counting it separately would hide exactly the thing
            // being looked for.
            ++total_blocks_;
            live_bytes_ = live_bytes_ + new_size - old_size;
        }
        return block;
    }

    void do_deallocate(void* pointer, cy::usize size, cy::usize alignment) noexcept override {
        if (pointer != nullptr && size <= live_bytes_) {
            live_bytes_ -= size;
        }
        upstream_.deallocate(pointer, size, alignment);
    }

private:
    cy::Allocator& upstream_;
    cy::u64 total_blocks_ = 0;
    cy::u64 live_bytes_ = 0;
};

constexpr cy::u32 kMillion = 1'000'000;

}  // namespace

CY_TEST_CASE("a million points after one reserve allocate nothing further") {
    CountingAllocator counting;
    PointSet points(counting);
    const AttributeDecl columns[2] = {
        AttributeDecl{"pcg.density", AttributeId{1}, AttributeType::F32},
        AttributeDecl{"pcg.priority", AttributeId{2}, AttributeType::U64}};

    CY_REQUIRE(points.reserve(kMillion, cy::Span<const AttributeDecl>(columns, 2)));
    // Everything this point set will ever allocate happened above. Seven blocks: three position
    // arrays, the slots, the identities, the column vector and its two payloads — the exact number
    // is the allocator's business, so what is asserted is that it stops growing, not what it was.
    const cy::u64 after_reserve = counting.total_blocks();
    CY_CHECK_GT(after_reserve, 0u);

    for (cy::u32 index = 0; index < kMillion; ++index) {
        cy::Expected<cy::u32, cy::Error> at = points.add(
            static_cast<cy::f32>(index & 0xFFFFU), 0.0F, static_cast<cy::f32>(index >> 16U), index);
        CY_REQUIRE(at.has_value());
        points.set_f32(AttributeId{1}, *at, 0.5F);
        points.set_u64(AttributeId{2}, *at, index);
    }
    CY_CHECK_EQ(points.size(), kMillion);
    // THE REQUIREMENT. Not "few", not "amortised": zero.
    CY_CHECK_EQ(counting.total_blocks(), after_reserve);

    // And the storage is proportional to the points rather than to anything per-point: 28 bytes of
    // position, slot and identity plus 12 of columns is 40, which is what "tens of bytes" means.
    CY_CHECK_LT(points.bytes(), static_cast<cy::u64>(kMillion) * 64u);
}

CY_TEST_CASE("a filter over a million points compacts in place, allocating nothing") {
    CountingAllocator counting;
    PointSet points(counting);
    const AttributeDecl columns[1] = {
        AttributeDecl{"pcg.density", AttributeId{1}, AttributeType::F32}};
    CY_REQUIRE(points.reserve(kMillion, cy::Span<const AttributeDecl>(columns, 1)));
    for (cy::u32 index = 0; index < kMillion; ++index) {
        CY_REQUIRE(points.add(0.0F, 0.0F, 0.0F, index).has_value());
    }
    cy::Array<cy::u8> keep(counting);
    CY_REQUIRE(keep.resize(kMillion));
    for (cy::u32 index = 0; index < kMillion; ++index) {
        keep[index] = static_cast<cy::u8>(index % 3 == 0 ? 1 : 0);
    }
    const cy::u64 before = counting.total_blocks();
    CY_REQUIRE(points.retain(cy::Span<const cy::u8>(keep.data(), kMillion)));
    // A filter that built a second point set would allocate one per region per regeneration, which
    // is the per-point allocation the requirement forbids wearing a different hat.
    CY_CHECK_EQ(counting.total_blocks(), before);
    CY_CHECK_EQ(points.size(), (kMillion + 2) / 3);
    // The survivors kept their ORIGINAL slots, which is what stops a rank among survivors being
    // invented here by accident.
    CY_CHECK_EQ(points.slot(1), 3u);
    CY_CHECK_EQ(points.slot(100), 300u);
}

CY_TEST_CASE("provenance switched off holds nothing at all") {
    // "Provenance SHALL be strippable from shipping builds." Held, not merely empty:
    // `RegionProvenance::bytes()` reports CAPACITY, so an array that was reserved and left empty
    // reports the cost it is actually carrying.
    const FlatSpatialQuery surface;
    constexpr cy::i32 kEdge = 4;

    GenerationWorld recorded(test::allocator(), test::extent_of(kEdge));
    cy::Expected<Generator, cy::Error> a = test::make_generator(recorded);
    CY_REQUIRE(a.has_value());
    GenerationContext with = test::context_of(1234, surface);
    with.provenance = ProvenanceMode::On;
    CY_REQUIRE(a->generate_all(ExecutionDomain::Cook, with).has_value());

    GenerationWorld stripped(test::allocator(), test::extent_of(kEdge));
    cy::Expected<Generator, cy::Error> b = test::make_generator(stripped);
    CY_REQUIRE(b.has_value());
    GenerationContext without = test::context_of(1234, surface);
    without.provenance = ProvenanceMode::Off;
    CY_REQUIRE(b->generate_all(ExecutionDomain::Cook, without).has_value());

    cy::u64 recorded_bytes = 0;
    cy::u64 stripped_bytes = 0;
    for (cy::i32 z = 0; z < kEdge; ++z) {
        for (cy::i32 x = 0; x < kEdge; ++x) {
            const cy::pcg::RegionState* left = recorded.find(RegionCoord{x, z, 0});
            const cy::pcg::RegionState* right = stripped.find(RegionCoord{x, z, 0});
            CY_REQUIRE(left != nullptr);
            CY_REQUIRE(right != nullptr);
            recorded_bytes += left->provenance.bytes();
            stripped_bytes += right->provenance.bytes();
        }
    }
    CY_CHECK_GT(recorded_bytes, 0u);
    CY_CHECK_EQ(stripped_bytes, 0u);

    // AND THE OUTPUT IS THE SAME. Stripping an explanation must not change what was generated, or
    // the shipping build would be a different world from the one the editor showed.
    CY_CHECK_EQ(test::world_digest(recorded), test::world_digest(stripped));
}
