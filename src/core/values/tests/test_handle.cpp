// Generational handles: packing, the stale-handle rejection, and tag separation. Task 1.3.2.
//
// `core-type-system` — "Generational handles". The scenario that matters most is
// "Stale handle is detected": a handle used after its object has been freed and the slot reused
// must fail the generation comparison and resolve to nothing, rather than aliasing the new
// occupant. That is the test that would have caught the bug the counter exists to prevent, so it
// is written against the exact sequence — allocate, keep the handle, free, allocate again into the
// same slot, and then ask.

#include <cy/core/values/handle.h>

#include <cy/test/test.h>

namespace {

CY_HANDLE_TAG(Mesh);
CY_HANDLE_TAG(Texture);

using MeshHandle = cy::Handle<MeshTag>;
using TextureHandle = cy::Handle<TextureTag>;

}  // namespace

CY_TEST_CASE("Handle: packs a slot and a generation into 64 bits") {
    const MeshHandle handle = MeshHandle::from_slot(7, 3);
    CY_CHECK_EQ(handle.index(), 7u);
    CY_CHECK_EQ(handle.generation(), 3u);
    CY_CHECK_EQ(handle.bits(), (3ull << 32) | 7ull);
    CY_CHECK(MeshHandle::from_bits(handle.bits()) == handle);
}

CY_TEST_CASE("Handle: a zeroed handle is null, not a reference to slot zero") {
    const MeshHandle unset;
    CY_CHECK(unset.is_null());
    CY_CHECK_FALSE(static_cast<bool>(unset));
    CY_CHECK_EQ(unset.bits(), 0u);
    // Slot 0 with a live generation is a real handle; only generation 0 is null.
    CY_CHECK_FALSE(MeshHandle::from_slot(0, 1).is_null());
}

CY_TEST_CASE("GenerationTable: a stale handle is rejected after the slot is reused") {
    cy::GenerationTable table(64);

    const cy::Expected<MeshHandle, cy::Error> first = table.allocate_handle<MeshTag>();
    CY_REQUIRE(first.has_value());
    const MeshHandle stale = *first;
    CY_CHECK(table.is_live(stale));

    CY_REQUIRE(table.release(stale).has_value());
    // Freed: the handle is already stale, before anything reuses the slot.
    CY_CHECK_FALSE(table.is_live(stale));

    // Reallocate. The free list hands back the same slot, which is precisely the case a bare index
    // would alias.
    const cy::Expected<MeshHandle, cy::Error> second = table.allocate_handle<MeshTag>();
    CY_REQUIRE(second.has_value());
    const MeshHandle fresh = *second;

    CY_CHECK_EQ(fresh.index(), stale.index());
    CY_CHECK_NE(fresh.generation(), stale.generation());
    CY_CHECK(table.is_live(fresh));
    CY_CHECK_FALSE(table.is_live(stale));
    CY_CHECK(table.stale_rejections() >= 2);
}

CY_TEST_CASE("GenerationTable: generations keep increasing across many reuses") {
    cy::GenerationTable table(64);
    cy::u32 previous = 0;
    for (int cycle = 0; cycle < 16; ++cycle) {
        const cy::Expected<cy::u32, cy::Error> slot = table.allocate();
        CY_REQUIRE(slot.has_value());
        const cy::u32 generation = table.generation_of(*slot);
        // Never zero, never even, and never a value a previous cycle handed out for this slot.
        CY_CHECK_NE(generation, 0u);
        CY_CHECK_EQ(generation % 2u, 1u);
        CY_CHECK_GT(generation, previous);
        previous = generation;
        CY_REQUIRE(table.release(*slot).has_value());
    }
}

CY_TEST_CASE("GenerationTable: releasing twice is reported, not ignored") {
    cy::GenerationTable table(64);
    const cy::Expected<cy::u32, cy::Error> slot = table.allocate();
    CY_REQUIRE(slot.has_value());

    CY_CHECK(table.release(*slot).has_value());
    const cy::Status again = table.release(*slot);
    CY_REQUIRE_FALSE(again.has_value());
    CY_CHECK(again.error().code == cy::ErrorCode::NotFound);

    const cy::Status never = table.release(9999);
    CY_REQUIRE_FALSE(never.has_value());
    CY_CHECK(never.error().code == cy::ErrorCode::OutOfRange);
}

CY_TEST_CASE("GenerationTable: live and capacity track allocation") {
    cy::GenerationTable table(64);
    CY_CHECK_EQ(table.live(), 0u);

    cy::u32 slots[4] = {};
    for (auto& slot : slots) {
        const cy::Expected<cy::u32, cy::Error> allocated = table.allocate();
        CY_REQUIRE(allocated.has_value());
        slot = *allocated;
    }
    CY_CHECK_EQ(table.live(), 4u);
    CY_CHECK_EQ(table.capacity(), 4u);

    CY_REQUIRE(table.release(slots[1]).has_value());
    CY_CHECK_EQ(table.live(), 3u);
    CY_CHECK_EQ(table.capacity(), 4u);  // the slot is free, not gone
}

CY_TEST_CASE("GenerationTable: growth across chunks keeps every handle live") {
    // Two slots per chunk is rounded up to the 64-slot floor, so this crosses several chunks.
    cy::GenerationTable table(64);
    cy::Expected<MeshHandle, cy::Error> handles[200] = {};
    for (auto& handle : handles) {
        handle = table.allocate_handle<MeshTag>();
        CY_REQUIRE(handle.has_value());
    }
    for (const auto& handle : handles) {
        CY_CHECK(table.is_live(*handle));
    }
    CY_CHECK_GT(table.capacity(), 64u);
}

CY_TEST_CASE("AnyHandle: erasing keeps the tag, and reading back the wrong one fails") {
    const MeshHandle mesh = MeshHandle::from_slot(3, 1);
    const cy::AnyHandle erased = cy::to_any(mesh);

    const cy::Expected<MeshHandle, cy::Error> recovered = cy::from_any<MeshTag>(erased);
    CY_REQUIRE(recovered.has_value());
    CY_CHECK(*recovered == mesh);

    const cy::Expected<TextureHandle, cy::Error> wrong = cy::from_any<TextureTag>(erased);
    CY_REQUIRE_FALSE(wrong.has_value());
    CY_CHECK(wrong.error().code == cy::ErrorCode::InvalidArgument);
}

CY_TEST_CASE("Handle tags carry a name for diagnostics") {
    CY_CHECK_NE(cy::handle_tag_id<MeshTag>(), cy::handle_tag_id<TextureTag>());
    CY_CHECK_EQ(cy::handle_tag_name(cy::handle_tag_id<MeshTag>()).text(), std::string_view("Mesh"));
    CY_CHECK_EQ(cy::handle_tag_name(cy::handle_tag_id<TextureTag>()).text(),
                std::string_view("Texture"));
}

CY_TEST_CASE("EntityId is a distinct 64-bit identity, not a handle") {
    const cy::EntityId entity = cy::EntityId::from_slot(5, 2);
    CY_CHECK_EQ(entity.index(), 5u);
    CY_CHECK_EQ(entity.generation(), 2u);
    CY_CHECK_FALSE(entity.is_null());
    CY_CHECK(cy::EntityId().is_null());

    // The type system keeps these apart; the compile-fail suite proves the assignment is an error.
    static_assert(!std::is_same_v<cy::EntityId, MeshHandle>);
}
