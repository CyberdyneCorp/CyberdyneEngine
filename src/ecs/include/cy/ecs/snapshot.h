#pragma once
// World serialization and snapshots. Task 2.10.
//
// `ecs-core` — "World serialization and snapshots" asks for two different things, and they are two
// different functions here because they answer different questions:
//
//   Snapshot   a fast in-memory copy for rollback, editor play-mode reset and deterministic
//              testing. It goes back into the world it came from, so entity ids are preserved
//              verbatim and nothing is remapped. "The world SHALL be restored from the snapshot
//              taken on entry, with no reliance on systems undoing their own mutations" — so the
//              restore replaces the world's contents rather than replaying anything.
//
//   serialize  a byte stream for all or a filtered subset of entities, self-describing, with
//              "entity references remapped to stable local ids". It may be read back into a
//              different world, so references are dense indices into the stream rather than the
//              ids of a world the reader has never seen.
//
// HOW A REFERENCE IS FOUND. Not by asking reflection what each field means, per row. A component
// declares the byte offsets of its `Entity` fields when it is registered (component.h), so
// remapping is a strided pass over known columns. The M2 spike measured the reflection-driven
// alternative at 4.7-5.2x and named it as the thing that would turn a cooked cell's activation into
// the walk the archetype layout exists to avoid; this is the same table, one level up.
//
// WHAT IS NOT HERE. The text form. `serialization-and-prefabs` puts both forms behind one
// reflection-driven traversal in src/core/serialize/ and src/scene/serialization/, and the ECS's
// job is to expose the entity data and the reference sites — which is what this file does. A second
// text writer here would be the second implementation that design.md §6 exists to prevent.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/ecs/world.h>

namespace cy::ecs {

/// An in-memory copy of a world's contents.
///
/// Not a byte stream: the entity table is copied as records, and each archetype's chunk columns are
/// copied as bytes. A buffer component's heap spill is deep-copied rather than having its pointer
/// copied, because two owners of one block is the bug this class would otherwise introduce into
/// every play-mode reset.
class Snapshot {
public:
    explicit Snapshot(Allocator& allocator) noexcept;
    ~Snapshot();

    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;

    /// Copy the world. Refuses while the world is being iterated: a snapshot of a world mid-query
    /// is a snapshot of a world about to change.
    [[nodiscard]] Status capture(World& world) noexcept;

    /// Replace the world's contents with the captured ones. The world must be the one captured
    /// from, or one whose component registry agrees with it — component ids are positions in a
    /// registry, and a snapshot restored against a different registration order would be wrong
    /// rather than refused.
    [[nodiscard]] Status restore(World& world) const noexcept;

    [[nodiscard]] bool empty() const noexcept { return archetypes_.empty(); }
    [[nodiscard]] u64 bytes() const noexcept;
    [[nodiscard]] u64 entity_count() const noexcept { return entity_count_; }

private:
    /// One archetype's rows, gathered out of its chunks into one contiguous copy per column.
    struct ArchetypeCopy {
        Array<ComponentTypeId> components;
        Array<SharedValue> shared;
        Array<Entity> keys;
        /// Column data, concatenated in column order; `column_offsets` says where each begins.
        Array<u8> columns;
        Array<usize> column_offsets;
        /// Buffer components' elements, concatenated. `buffer_sizes` holds one entry per
        /// (buffer column, row) in column-major order.
        Array<u8> buffer_data;
        Array<u32> buffer_sizes;
        u32 rows = 0;

        explicit ArchetypeCopy(Allocator& allocator) noexcept
            : components(allocator),
              shared(allocator),
              keys(allocator),
              columns(allocator),
              column_offsets(allocator),
              buffer_data(allocator),
              buffer_sizes(allocator) {}
    };

    struct SparseCopy {
        ComponentTypeId component = kInvalidComponent;
        Array<u32> keys;
        Array<u32> generations;
        Array<u8> values;

        explicit SparseCopy(Allocator& allocator) noexcept
            : keys(allocator), generations(allocator), values(allocator) {}
    };

    struct SharedCopy {
        ComponentTypeId component = kInvalidComponent;
        u32 value_size = 0;
        u32 count = 0;
        Array<u8> bytes;

        explicit SharedCopy(Allocator& allocator) noexcept : bytes(allocator) {}
    };

    /// One archetype's rows, gathered into `copy`. Split out of `capture` because it is the whole
    /// of the chunk walk and `capture` is the four flat copies around it.
    [[nodiscard]] static Status capture_archetype(const ComponentRegistry& registry,
                                                  Archetype& archetype,
                                                  ArchetypeCopy& copy) noexcept;
    /// One column of one archetype, across every chunk, plus whatever its buffers spilled.
    [[nodiscard]] static Status capture_column(Archetype& archetype, const ComponentInfo& info,
                                               u32 column, ArchetypeCopy& copy) noexcept;
    /// The heap spills of one chunk's buffer column. Nothing for any other kind.
    [[nodiscard]] static Status capture_spills(ChunkView& view, const ComponentInfo& info,
                                               u32 column, ArchetypeCopy& copy) noexcept;

    [[nodiscard]] Status capture_sparse(World& world) noexcept;
    [[nodiscard]] Status capture_shared(World& world) noexcept;
    [[nodiscard]] Status capture_resources(World& world) noexcept;
    [[nodiscard]] Status restore_sparse(World& world) const noexcept;
    [[nodiscard]] Status restore_shared(World& world) const noexcept;
    void restore_resources(World& world) const noexcept;

    /// The sparse side tables, the interned shared values and the resources. Split out of
    /// `capture` and `restore` because they are three flat copies with nothing in common with the
    /// chunk walk above them, and inlining them made one function that did four things.
    [[nodiscard]] Status capture_side_tables(World& world) noexcept;
    [[nodiscard]] Status restore_side_tables(World& world) const noexcept;

    /// Copy one archetype's captured columns back into freshly added rows. Static: everything it
    /// needs is in the copy it is given, and a member function that reads no member is one a reader
    /// has to check.
    [[nodiscard]] static Status write_rows(World& world, Archetype& archetype,
                                           const ArchetypeCopy& copy,
                                           Span<const Archetype::RowRange> ranges) noexcept;

    /// Rebuild the entity table's location records from the restored chunks' key columns.
    [[nodiscard]] static Status reindex(World& world) noexcept;

    Allocator* allocator_;
    Array<ArchetypeCopy> archetypes_;
    Array<SparseCopy> sparse_;
    Array<SharedCopy> shared_;
    Array<EntityTable::Record> records_;
    Array<u32> free_indices_;
    /// Every resource's bytes, concatenated in resource id order. A resource is world state and a
    /// play-mode reset has to put it back; the registry on the other side is the same one, so the
    /// sizes are read from it rather than stored twice.
    Array<u8> resources_;
    u32 live_ = 0;
    u64 entity_count_ = 0;
    u64 version_ = 1;
};

/// What `serialize` writes and `deserialize` reads. Bumped when the layout changes; a stream with a
/// different number is refused rather than misread.
inline constexpr u32 kWorldStreamVersion = 1;

/// Serialize entities and their components into a self-describing byte stream.
///
/// `subset` empty means every live entity. Otherwise only the named ones are written, and a
/// reference to an entity outside the subset is written as the null entity — a dangling reference
/// is made explicit at write time rather than resolving to whatever occupies that id later.
[[nodiscard]] Status serialize(World& world, Array<u8>& out,
                               Span<const Entity> subset = {}) noexcept;

/// Read a stream back into a world, appending the entities it created to `out` in stream order.
///
/// The world must already have registered every component the stream names; a stream naming one it
/// has not is refused with the component's name rather than loaded with a hole in it.
[[nodiscard]] Status deserialize(World& world, Span<const u8> bytes, Array<Entity>& out) noexcept;

}  // namespace cy::ecs
