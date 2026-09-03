// Handle pools and chunked storage. Tasks 2.5 and 2.6.

#include <cy/test/test.h>

#include <cy/core/memory/chunk_storage.h>
#include <cy/core/memory/handle_pool.h>

#include <cstring>
#include <type_traits>

namespace {

CY_HANDLE_TAG(Mesh);
CY_HANDLE_TAG(Texture);

struct MeshResource {
    int vertices = 0;
    cy::u64 padding[7] = {};
};

}  // namespace

CY_TEST_CASE("pointer stability: a pool grows and an existing T* stays valid") {
    cy::HandlePool<MeshResource, MeshTag> pool(cy::MemoryDomain::Renderer, "meshes", 16);

    auto first = pool.create();
    CY_REQUIRE(first.has_value());
    MeshResource* held = pool.resolve(*first);
    CY_REQUIRE(held != nullptr);
    held->vertices = 1234;

    // Far past the first chunk, so several more have been committed.
    cy::Handle<MeshTag> handles[200];
    for (auto& handle : handles) {
        auto created = pool.create();
        CY_REQUIRE(created.has_value());
        handle = *created;
    }
    CY_CHECK(pool.chunk_count() > 1u);
    CY_CHECK_EQ(pool.size(), 201u);

    CY_CHECK_EQ(pool.resolve(*first), held);  // the pointer did not move
    CY_CHECK_EQ(held->vertices, 1234);
}

CY_TEST_CASE("a stale handle resolves to nothing rather than to the slot's new occupant") {
    cy::HandlePool<MeshResource, MeshTag> pool(cy::MemoryDomain::Renderer, "meshes", 8);

    auto original = pool.create();
    CY_REQUIRE(original.has_value());
    const cy::Handle<MeshTag> stale = *original;
    MeshResource* const original_object = pool.resolve(stale);
    CY_REQUIRE(original_object != nullptr);
    original_object->vertices = 7;

    CY_REQUIRE(pool.destroy(stale).has_value());
    CY_CHECK(pool.resolve(stale) == nullptr);
    // Destroying it twice is reported rather than corrupting the free list.
    CY_CHECK_FALSE(pool.destroy(stale).has_value());

    auto replacement = pool.create();
    CY_REQUIRE(replacement.has_value());
    CY_CHECK_EQ(replacement->index(), stale.index());            // the slot was reused
    CY_CHECK_NE(replacement->generation(), stale.generation());  // and the generation moved on
    CY_CHECK(pool.resolve(stale) == nullptr);  // so the old handle still resolves to nothing
    CY_CHECK(pool.resolve(*replacement) != nullptr);
    CY_CHECK(pool.stale_rejections() > 0);
}

CY_TEST_CASE("two pools with different tags are different types") {
    static_assert(!std::is_same_v<cy::Handle<MeshTag>, cy::Handle<TextureTag>>,
                  "a handle's tag is what stops one pool's handle reaching another's");
    cy::HandlePool<MeshResource, MeshTag> meshes(cy::MemoryDomain::Renderer, "meshes", 8);
    auto handle = meshes.create();
    CY_REQUIRE(handle.has_value());
    CY_CHECK(std::strcmp(meshes.tag(), "meshes") == 0);
}

CY_TEST_CASE("chunk capacity derives from the column set and is recorded") {
    // Sizes and alignments only — this layer has no idea what a column holds.
    const cy::ColumnSpec columns[] = {
        {4, 4},    // a 4-byte column
        {12, 4},   // a 12-byte column
        {16, 16},  // a 16-byte column that forces padding before it
    };
    const auto layout =
        cy::ChunkLayout::compute(16 * 1024, 8, 8, cy::Span<const cy::ColumnSpec>(columns, 3));
    CY_REQUIRE(layout.has_value());

    CY_CHECK_EQ(layout->column_count(), 3u);
    CY_CHECK_EQ(layout->chunk_bytes(), 16u * 1024u);
    CY_CHECK(layout->capacity() > 0u);
    CY_CHECK(layout->used_bytes() <= 16u * 1024u);

    // It is the LARGEST count that fits: one more row would not.
    const cy::u64 row_bytes = 8 + 4 + 12 + 16;
    CY_CHECK(static_cast<cy::u64>(layout->capacity() + 1) * row_bytes > 16u * 1024u - 64u);

    // Every column is aligned to its own requirement, and the key array comes first.
    CY_CHECK(layout->key_offset() < layout->column_offset(0));
    CY_CHECK_EQ(layout->column_offset(0) % 4u, 0u);
    CY_CHECK_EQ(layout->column_offset(2) % 16u, 0u);

    // A row larger than the chunk is a configuration error, not a capacity of zero.
    const cy::ColumnSpec huge[] = {{100000, 4}};
    CY_CHECK_FALSE(
        cy::ChunkLayout::compute(1024, 8, 8, cy::Span<const cy::ColumnSpec>(huge, 1)).has_value());
}

CY_TEST_CASE("iteration is linear: a column is one contiguous span") {
    const cy::ColumnSpec columns[] = {{sizeof(cy::f32), alignof(cy::f32)},
                                      {sizeof(cy::u32), alignof(cy::u32)}};
    const auto layout = cy::ChunkLayout::compute(4096, sizeof(cy::u64), alignof(cy::u64),
                                                 cy::Span<const cy::ColumnSpec>(columns, 2));
    CY_REQUIRE(layout.has_value());

    cy::ChunkStore store(*layout, cy::MemoryDomain::Ecs, "rows");
    const cy::u32 rows = layout->capacity();
    for (cy::u32 index = 0; index < rows; ++index) {
        const auto placed = store.add_row();
        CY_REQUIRE(placed.has_value());
        CY_CHECK_EQ(placed->chunk, 0u);
        cy::ChunkView chunk = store.chunk(0);
        chunk.keys_as<cy::u64>()[placed->row] = index;
        chunk.column_as<cy::f32>(0)[placed->row] = static_cast<cy::f32>(index);
        chunk.column_as<cy::u32>(1)[placed->row] = index * 2;
    }
    CY_CHECK_EQ(store.chunk_count(), 1u);
    CY_CHECK_EQ(store.row_count(), rows);

    // One more row takes a second chunk rather than growing the first.
    const auto overflowed = store.add_row();
    CY_REQUIRE(overflowed.has_value());
    CY_CHECK_EQ(overflowed->chunk, 1u);
    CY_CHECK_EQ(store.chunk_count(), 2u);

    cy::ChunkView first = store.chunk(0);
    const cy::Span<cy::f32> floats = first.column_as<cy::f32>(0);
    CY_REQUIRE_EQ(floats.size(), rows);
    // Contiguous: the distance between consecutive elements is one element, with no indirection.
    CY_CHECK_EQ(&floats[1] - &floats[0], 1);
    cy::f32 sum = 0.0f;
    for (cy::f32 value : floats) {
        sum += value;
    }
    CY_CHECK_EQ(sum, static_cast<cy::f32>(rows) * static_cast<cy::f32>(rows - 1) / 2.0f);
}

CY_TEST_CASE("change detection is O(1) per chunk per column") {
    const cy::ColumnSpec columns[] = {{4, 4}, {4, 4}};
    const auto layout =
        cy::ChunkLayout::compute(4096, 8, 8, cy::Span<const cy::ColumnSpec>(columns, 2));
    CY_REQUIRE(layout.has_value());

    cy::ChunkStore store(*layout, cy::MemoryDomain::Ecs, "versions");
    CY_REQUIRE(store.add_row().has_value());

    const cy::u64 observed = store.current_version();
    CY_CHECK_FALSE(store.chunk(0).changed_since(0, observed));
    CY_CHECK_FALSE(store.chunk(0).changed_since(1, observed));

    const cy::u64 stamped = store.touch(0, 1);
    CY_CHECK(stamped > observed);
    CY_CHECK_FALSE(store.chunk(0).changed_since(0, observed));  // column 0 did not change
    CY_CHECK(store.chunk(0).changed_since(1, observed));        // column 1 did
}

CY_TEST_CASE("removing a row moves the last one into the gap and says where it came from") {
    const cy::ColumnSpec columns[] = {{4, 4}};
    const auto layout =
        cy::ChunkLayout::compute(1024, 8, 8, cy::Span<const cy::ColumnSpec>(columns, 1));
    CY_REQUIRE(layout.has_value());

    cy::ChunkStore store(*layout, cy::MemoryDomain::Ecs, "rows");
    for (cy::u32 index = 0; index < 4; ++index) {
        const auto placed = store.add_row();
        CY_REQUIRE(placed.has_value());
        cy::ChunkView chunk = store.chunk(placed->chunk);
        chunk.keys_as<cy::u64>()[placed->row] = 100 + index;
        chunk.column_as<cy::u32>(0)[placed->row] = index;
    }

    const cy::ChunkStore::RowMove move = store.remove_row(0, 1);
    CY_CHECK(move.moved);
    CY_CHECK_EQ(move.from_row, 3u);
    CY_CHECK_EQ(move.to_row, 1u);
    CY_CHECK_EQ(store.row_count(), 3u);
    CY_CHECK_EQ(store.chunk(0).keys_as<cy::u64>()[1], 103u);
    CY_CHECK_EQ(store.chunk(0).column_as<cy::u32>(0)[1], 3u);

    // Removing the last row moves nothing.
    const cy::ChunkStore::RowMove tail = store.remove_row(0, 2);
    CY_CHECK_FALSE(tail.moved);

    CY_CHECK_EQ(store.trim(), 0u);  // the chunk still holds rows
    (void)store.remove_row(0, 1);
    (void)store.remove_row(0, 0);
    CY_CHECK_EQ(store.row_count(), 0u);
    CY_CHECK_EQ(store.trim(), 1u);
    CY_CHECK_EQ(store.chunk_count(), 0u);
}
