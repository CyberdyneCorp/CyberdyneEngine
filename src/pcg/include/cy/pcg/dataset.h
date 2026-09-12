#pragma once
// Typed spatial datasets, and attributes addressed by compiled identifiers. M10 task 4.1.
//
// `procedural-content-generation` — "Typed spatial datasets": generation "SHALL operate on typed
// spatial datasets, not on arbitrary engine objects", a generator "SHALL be a transformation from
// datasets to datasets ... SHALL NOT be a procedure that directly creates runtime objects", and
// "dataset elements SHALL carry typed attributes addressed by COMPILED IDENTIFIERS ... Attribute
// names SHALL NOT be resolved by string lookup at execution time."
//
// ================================================================================================
// WHERE THE COMPILED IDENTIFIER COMES FROM, AND WHY IT IS NOT `reflect::FieldId`
// ================================================================================================
//
// `core-type-system`'s identity mechanism is an opaque number assigned on first sight and recorded
// in `identity/manifest.toml` — a persistent identity that survives a rename, for a type the engine
// serialises. A PCG attribute is not that: `density`, `slope_weight` and `priority` exist only
// inside one compiled program, are named by the graph that declares them, and die with it. Minting
// a manifest entry per graph attribute would put a project's authoring vocabulary into the engine's
// permanent identity record, which is the thing that file exists to keep small.
//
// So the mechanism is the same SHAPE — a number assigned once, resolved never at execution — with
// the scope of a program: `AttributeTable` interns a name to an `AttributeId` at GRAPH-COMPILE
// time, and the compiled program carries the id. `AttributeTable::find_by_name()` exists and is
// declared compile-time only in as many words; nothing in `execute.h` calls it, and the suite in
// tests/test_dataset.cpp holds that as a case by walking the compiled program for a string compare.
//
// ================================================================================================
// NO PER-POINT HEAP ALLOCATION
// ================================================================================================
//
// `procedural-content-generation` — "PCG performance": "WHEN a region generates millions of
// candidates THEN no per-point heap allocation SHALL occur." `PointSet` is therefore structure of
// arrays with a capacity RESERVED ONCE, and `add()` past that capacity is refused rather than
// growing — a growing container would allocate, and the refusal is what makes the property
// checkable instead of merely likely. `tests/test_dataset.cpp` measures it against a
// `TrackingAllocator`: a million points after one `reserve()` is zero further allocations.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>

namespace cy::pcg {

// --- Attributes ----------------------------------------------------------------------------------

/// A compiled attribute identifier: an index into the program's own attribute table.
///
/// Opaque at execution. Zero is the null identifier and is never assigned, so a default-constructed
/// id is invalid rather than being the first column's.
struct AttributeId {
    u16 value = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(AttributeId, AttributeId) noexcept = default;
};

/// The shapes an attribute column carries. Deliberately few: every one of them has a storage width,
/// a comparison and a deterministic encoding into a digest, and a shape that had none of those
/// would be a shape the cache key could not cover.
enum class AttributeType : u8 {
    F32 = 0,
    F32x3,
    I32,
    U64,
    Bool,
};

[[nodiscard]] const char* attribute_type_name(AttributeType type) noexcept;

/// Bytes one element of `type` occupies in a column.
[[nodiscard]] constexpr u32 attribute_bytes(AttributeType type) noexcept {
    switch (type) {
        case AttributeType::F32:
            return 4;
        case AttributeType::F32x3:
            return 12;
        case AttributeType::I32:
            return 4;
        case AttributeType::U64:
            return 8;
        case AttributeType::Bool:
            return 1;
    }
    return 0;
}

/// One attribute's declaration, as the compiler records it.
struct AttributeDecl {
    const char* name = "";
    AttributeId id;
    AttributeType type = AttributeType::F32;
};

/// The name-to-id assignment for one compiled program.
///
/// Interning happens at graph-compile time and nowhere else. The table is carried into the program
/// so that a diagnostic can print a name, which is the only reason the strings survive compilation
/// at all — `provenance` and the profiler both need them, and neither is on a hot path.
class AttributeTable {
public:
    explicit AttributeTable(Allocator& allocator) noexcept : entries_(allocator) {}

    AttributeTable(const AttributeTable&) = delete;
    AttributeTable& operator=(const AttributeTable&) = delete;
    AttributeTable(AttributeTable&&) noexcept = default;
    AttributeTable& operator=(AttributeTable&&) noexcept = default;

    /// Assign `name` an identifier, or return the one it already has. Re-interning a name with a
    /// DIFFERENT type is refused: two nodes that disagree about whether `density` is a float or a
    /// category is a graph defect, and resolving it by last-writer-wins would make the compiled
    /// column's meaning depend on node order.
    [[nodiscard]] Expected<AttributeId, Error> intern(const char* name,
                                                      AttributeType type) noexcept;

    /// COMPILE TIME ONLY. Nothing under `execute.h` calls this, and the dataset suite proves it.
    [[nodiscard]] const AttributeDecl* find_by_name(const char* name) const noexcept;

    [[nodiscard]] const AttributeDecl* find(AttributeId id) const noexcept;
    [[nodiscard]] const char* name_of(AttributeId id) const noexcept;
    [[nodiscard]] usize size() const noexcept { return entries_.size(); }
    [[nodiscard]] Span<const AttributeDecl> entries() const noexcept { return entries_.span(); }

    [[nodiscard]] Expected<AttributeTable, Error> clone() const noexcept;

private:
    Array<AttributeDecl> entries_;
};

// --- Datasets ------------------------------------------------------------------------------------

/// The dataset kinds the specification names. A generator is a transformation between these and
/// nothing else; an engine object is not one of them, which is the requirement.
enum class DatasetKind : u8 {
    PointSet = 0,
    Volume,
    Surface,
    Spline,
    Field,
    Geometry,
    AttributeTable,
    EntitySet,
    Regions,
    Raster,
    kCount,
};

[[nodiscard]] const char* dataset_kind_name(DatasetKind kind) noexcept;

/// A point set: positions in region-local metres, plus typed attribute columns.
///
/// Positions are region-local `f32` rather than `world::WorldVec3d`, for the reason
/// `world/coordinates.h` gives for the cell-relative form: a region is at most a few hundred metres
/// across, so f32 is exact to well under a millimetre inside it, and a region's output is then
/// identical wherever in the world the region sits — which is what makes a cached region
/// relocatable and its digest comparable.
class PointSet {
public:
    explicit PointSet(Allocator& allocator) noexcept
        : allocator_(&allocator),
          x_(allocator),
          y_(allocator),
          z_(allocator),
          slots_(allocator),
          identities_(allocator),
          columns_(allocator) {}

    PointSet(const PointSet&) = delete;
    PointSet& operator=(const PointSet&) = delete;
    PointSet(PointSet&&) noexcept = default;
    PointSet& operator=(PointSet&&) noexcept = default;

    /// Reserve room for `points` points and declare the columns. Called once per region, before any
    /// point exists. Every allocation this class will ever make happens here.
    [[nodiscard]] Status reserve(usize points, Span<const AttributeDecl> columns) noexcept;

    /// Add a point. Refused — never grown — once `capacity()` is reached, because growing is the
    /// per-point heap allocation the specification forbids.
    ///
    /// `slot` is the candidate's own index in the generating node's sequence, BEFORE any rejection.
    /// It is what identity is derived from, and it is stored rather than recomputed because a
    /// filtered point set no longer knows its own position in the original sequence — which is
    /// exactly how a rank-among-survivors identity gets invented by accident.
    [[nodiscard]] Expected<u32, Error> add(f32 x, f32 y, f32 z, u32 slot) noexcept;

    void clear() noexcept;

    /// Compact in place, keeping the points whose `keep` byte is non-zero.
    ///
    /// In place, and that is the requirement rather than an optimisation: a filter that built a
    /// second point set would allocate one per region per regeneration, which is the per-point heap
    /// allocation the specification forbids wearing a different hat. Slots and identities travel
    /// with their points, so a filtered set still knows each survivor's ORIGINAL slot — which is
    /// what stops a rank among survivors from being invented here by accident.
    [[nodiscard]] Status retain(Span<const u8> keep) noexcept;

    [[nodiscard]] usize size() const noexcept { return x_.size(); }
    [[nodiscard]] usize capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool empty() const noexcept { return x_.size() == 0; }

    [[nodiscard]] f32 x(usize index) const noexcept { return x_[index]; }
    [[nodiscard]] f32 y(usize index) const noexcept { return y_[index]; }
    [[nodiscard]] f32 z(usize index) const noexcept { return z_[index]; }
    [[nodiscard]] u32 slot(usize index) const noexcept { return slots_[index]; }

    void set_y(usize index, f32 value) noexcept { y_[index] = value; }
    /// Move a point without touching its slot or its identity. What an author's `Move` override
    /// does: the instance did not become a different instance by being dragged.
    void set_position(usize index, f32 x, f32 y, f32 z) noexcept {
        x_[index] = x;
        y_[index] = y;
        z_[index] = z;
    }

    /// The stable identity of point `index`. Assigned by the scatter node through
    /// `identity.h`'s derivation; never a counter and never a rank. Zero until assigned.
    [[nodiscard]] u64 identity(usize index) const noexcept { return identities_[index]; }
    void set_identity(usize index, u64 value) noexcept { identities_[index] = value; }

    /// Typed column access by compiled identifier. A linear walk over at most a handful of columns,
    /// which is cheaper than a hash and — more to the point — is not a string lookup.
    [[nodiscard]] f32 get_f32(AttributeId id, usize index) const noexcept;
    void set_f32(AttributeId id, usize index, f32 value) noexcept;
    [[nodiscard]] i32 get_i32(AttributeId id, usize index) const noexcept;
    void set_i32(AttributeId id, usize index, i32 value) noexcept;
    [[nodiscard]] u64 get_u64(AttributeId id, usize index) const noexcept;
    void set_u64(AttributeId id, usize index, u64 value) noexcept;

    [[nodiscard]] bool has_column(AttributeId id) const noexcept {
        return find_column(id) != nullptr;
    }
    [[nodiscard]] usize column_count() const noexcept { return columns_.size(); }

    /// Bytes this point set holds, for the profiler's per-node memory line.
    [[nodiscard]] u64 bytes() const noexcept;

    /// A deterministic digest of every position, slot, identity and column value, in index order.
    /// The comparison the cache and the invalidation fixed point are both built on, and the reason
    /// it is a function of the CONTENT rather than of the allocation is that a region regenerated
    /// into a different buffer must compare equal to the one it reproduces.
    [[nodiscard]] u64 digest() const noexcept;

    [[nodiscard]] Expected<PointSet, Error> clone() const noexcept;

private:
    struct Column {
        AttributeId id;
        AttributeType type = AttributeType::F32;
        Array<u8> data;

        explicit Column(Allocator& allocator) noexcept : data(allocator) {}
    };

    [[nodiscard]] Column* find_column(AttributeId id) noexcept;
    [[nodiscard]] const Column* find_column(AttributeId id) const noexcept;

    Allocator* allocator_;
    Array<f32> x_;
    Array<f32> y_;
    Array<f32> z_;
    Array<u32> slots_;
    Array<u64> identities_;
    Array<Column> columns_;
    usize capacity_ = 0;
};

/// A region-local raster: the intermediate every field-shaped node reads and writes.
///
/// Square, and its edge is a program-wide constant rather than a per-node one — see `execute.h`'s
/// `kRegionCells`. Channels are addressed by compiled identifier, like a point set's columns.
class Raster {
public:
    explicit Raster(Allocator& allocator) noexcept : channels_(allocator) {}

    Raster(const Raster&) = delete;
    Raster& operator=(const Raster&) = delete;
    Raster(Raster&&) noexcept = default;
    Raster& operator=(Raster&&) noexcept = default;

    /// Declare `edge * edge` cells and the channels over them. One allocation per channel, once.
    [[nodiscard]] Status reset(u32 edge, Span<const AttributeId> channels) noexcept;

    [[nodiscard]] u32 edge() const noexcept { return edge_; }
    [[nodiscard]] usize channel_count() const noexcept { return channels_.size(); }
    [[nodiscard]] bool has(AttributeId id) const noexcept { return find(id) != nullptr; }

    [[nodiscard]] f32 at(AttributeId id, u32 x, u32 z) const noexcept;
    void set(AttributeId id, u32 x, u32 z, f32 value) noexcept;

    [[nodiscard]] Span<const f32> values(AttributeId id) const noexcept;
    [[nodiscard]] Span<f32> values_mutable(AttributeId id) noexcept;

    [[nodiscard]] u64 digest() const noexcept;
    [[nodiscard]] u64 digest_of(AttributeId id) const noexcept;
    [[nodiscard]] u64 bytes() const noexcept;

    [[nodiscard]] Expected<Raster, Error> clone() const noexcept;

private:
    struct Channel {
        AttributeId id;
        Array<f32> values;

        explicit Channel(Allocator& allocator) noexcept : values(allocator) {}
    };

    [[nodiscard]] Channel* find(AttributeId id) noexcept;
    [[nodiscard]] const Channel* find(AttributeId id) const noexcept;

    Array<Channel> channels_;
    u32 edge_ = 0;
};

/// The digest every dataset in this module folds into. Order-sensitive by construction, because two
/// point sets holding the same points in a different order ARE different outputs — a generator that
/// emitted them in worker-completion order would be the defect this digest has to catch.
///
/// Deliberately not `cy::hash_bytes`: that function is seeded per process in development builds, so
/// a digest keyed by it would differ between two runs of the same binary. Fixed constants here, the
/// same numbers on every machine forever, for the reason `determinism/random.h` gives for its own.
class Digest {
public:
    constexpr Digest() = default;

    constexpr void u64_value(u64 value) noexcept {
        state_ = fold(state_ ^ value, kPrime0);
        state_ = fold(state_, kPrime1);
    }
    constexpr void u32_value(u32 value) noexcept { u64_value(static_cast<u64>(value)); }
    void f32_value(f32 value) noexcept;
    void bytes(const void* data, usize size) noexcept;

    [[nodiscard]] constexpr u64 value() const noexcept { return fold(state_, kPrime2); }

private:
    static constexpr u64 kPrime0 = 0x9e3779b97f4a7c15ULL;
    static constexpr u64 kPrime1 = 0xbf58476d1ce4e5b9ULL;
    static constexpr u64 kPrime2 = 0x94d049bb133111ebULL;

    [[nodiscard]] static constexpr u64 fold(u64 a, u64 b) noexcept {
        const __uint128_t product = static_cast<__uint128_t>(a) * static_cast<__uint128_t>(b);
        return static_cast<u64>(product) ^ static_cast<u64>(product >> 64U);
    }

    u64 state_ = 0xcbf29ce484222325ULL;
};

}  // namespace cy::pcg
