#pragma once
// The sparse tiled store: where a field's values live, how they are sampled, and the two paths a
// sample can take. M10 tasks 1.1, 1.3 and 1.4.
//
// `environment-fields` — "Sparse tiled storage and streaming": "Regions with no data SHALL consume
// no storage and SHALL sample the field's declared default", "A sample outside resident data SHALL
// return a defined result — the coarsest resident level, or the declared default — and SHALL NOT
// block or fault", and "Fields SHALL support multiple resolutions, so a consumer that needs coarse
// data does not force fine data resident."
//
// ================================================================================================
// TWO SAMPLING PATHS, AND WHY THAT IS NOT A SECOND MECHANISM
// ================================================================================================
//
// `sample()` walks the levels from finest to coarsest and returns the first that has data, with the
// level it came from. That is what the specification's residency requirement asks for — "A sample
// SHALL return the finest resident level at that position together with a resolution indicator, so
// a consumer can decide whether the answer is precise enough rather than assuming it is."
//
// **That answer depends on what streamed, and therefore gameplay may not use it.**
// "Determinism of gameplay-visible fields": a gameplay-visible field's value "SHALL depend only on
// cooked data, the world seed, and recorded persistent changes — never on frame timing, camera
// position, STREAMING ORDER, or GPU execution order." A value that is fine near the player and
// coarse away from them is a value that depends on where the player is.
//
// So `sample_deterministic()` reads ONE level — the field's declared `gameplay_level` — and returns
// the declared default when that level has no data there. Its answer is a function of the field's
// contents and nothing else, which is the requirement, and `FieldRegistry::declare()` refuses a
// gameplay-visible field whose `gameplay_level` is not declared `resident_everywhere`, so the
// answer is also not usually the default.
//
// This is one mechanism seen from the two sides `simulation-and-determinism` already distinguishes,
// not a second one: which path a reader gets is decided by `FieldReader`'s class, at open time,
// through `determinism::may_read()`.
//
// ================================================================================================
// WHAT A MISSING NEIGHBOUR TILE DOES, STATED RATHER THAN DISCOVERED
// ================================================================================================
//
// A linear sample reads up to eight lattice points, and they do not all live in one tile. The rule:
// the tile containing the SAMPLE POSITION decides whether the level resolved at all, and a corner
// whose own tile is not resident is CLAMPED to the resident tile's edge rather than dropped or
// defaulted. A sample therefore never faults, never blocks and never blends a value against a
// default it happens to sit next to — and the seam it can produce at a partially resident boundary
// is visible only on the `sample()` path, which is the presentation path, because the deterministic
// path reads a level that is resident everywhere.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/classification.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/environment/field.h>
#include <cy/world/coordinates.h>

namespace cy::environment {

/// Lattice points per tile edge, horizontally. A constant rather than a declaration: the tile is
/// the unit of residency, of versioning and of change notification, and a substrate whose tile size
/// varied per field would make "invalidate the region that changed" mean a different amount of
/// world for every consumer of it.
///
/// Sixteen, because it is the size at which a scalar `UNorm8` tile is 256 bytes and an f32 `Vec4`
/// tile is 4 KiB — a page — and because a 16x16 tile of a 1 m field is the 16 m granularity a
/// terrain deformation or a fire spread actually changes.
inline constexpr u32 kTileCells = 16;

/// Which tile, of which level, of which layer, of which field. The store's key.
///
/// There is no vertical tile coordinate: a field's vertical extent is a declared COLUMN
/// (`FieldDeclaration::vertical_cells`), resident whole. Worlds are thin vertically compared to
/// their horizontal extent, and a vertical tiling would triple the key's arithmetic to save storage
/// on a dimension that is already small.
struct TileAddress {
    FieldId field;
    i32 x = 0;
    i32 z = 0;
    u8 level = 0;
    u8 layer = 0;

    friend constexpr bool operator==(const TileAddress&, const TileAddress&) noexcept = default;

    [[nodiscard]] constexpr FieldResidency residency() const noexcept {
        return static_cast<FieldResidency>(level);
    }
    [[nodiscard]] constexpr FieldLayer field_layer() const noexcept {
        return static_cast<FieldLayer>(layer);
    }
};

/// A region's extent in absolute metres, as a change event carries it. f64 for the reason
/// `world::WorldVec3d` is f64: a bound a thousand kilometres out has 64 mm of f32 spacing, and a
/// consumer that invalidates a rectangle must not have to widen it to be safe.
struct FieldBounds {
    f64 min_x = 0.0;
    f64 min_z = 0.0;
    f64 max_x = 0.0;
    f64 max_z = 0.0;
};

[[nodiscard]] FieldBounds tile_bounds(const FieldDeclaration& declaration,
                                      const TileAddress& address) noexcept;

/// One sample, and how good it is.
///
/// `level` and `cell_metres` together are the specification's "resolution indicator": a consumer
/// deciding whether the answer is precise enough needs the metres, and one reporting why it is not
/// needs the level's name.
struct FieldSample {
    FieldValue value;
    FieldResidency level = FieldResidency::Macro;
    f32 cell_metres = 0.0F;
    /// False when nothing had data at this position and `value` is the field's declared default.
    bool resolved = false;
    /// The version of the region the value came from. "WHEN a consumer holds a derived value, THEN
    /// it SHALL detect staleness by version comparison rather than by re-reading the field" — this
    /// is the number that comparison is made against, and `version_of()` is how it is re-read
    /// without sampling.
    u64 version = 0;
    /// Lattice points read to produce this sample: one for a nearest sample, up to eight for a
    /// linear one in a volumetric field. The specification asks a field's diagnostics to report
    /// SAMPLE COST, and this is that cost counted rather than estimated.
    u64 reads = 0;
};

/// `environment-fields` — "Field diagnostics": "a point query showing the value and WHICH LAYER
/// CONTRIBUTED IT". The combined answer, the two contributions, and the producer's own name.
struct FieldPointQuery {
    FieldSample sample;
    FieldValue base;
    FieldValue delta;
    bool has_base = false;
    bool has_delta = false;
    /// The version of the tile the sample came from, so a consumer holding a derived value can
    /// detect staleness by comparing a number — "Staleness is cheap to detect".
    u64 version = 0;
    const char* producer = "";
    const char* field_name = "";
};

/// Per-field diagnostics. "resident tile extents and resolutions, memory in use, producer identity,
/// sample cost".
struct FieldDiagnostics {
    FieldId field;
    const char* field_name = "";
    const char* producer = "";
    u32 tiles[kFieldResidencyCount] = {};
    u64 bytes = 0;
    /// Extent of the resident tiles at the finest declared level, in tile coordinates. A
    /// world-scale visualisation draws this rectangle.
    i32 min_tile_x = 0;
    i32 min_tile_z = 0;
    i32 max_tile_x = 0;
    i32 max_tile_z = 0;
    /// Samples taken since the store was created, and lattice points read for them. The ratio is
    /// the sample cost the specification asks a field's diagnostics to report, and it is counted
    /// rather than estimated.
    u64 samples = 0;
    u64 lattice_reads = 0;
};

// --- Change events
// --------------------------------------------------------------------------------

/// What happened to a region. `environment-fields` — "Field versioning and change events": events
/// "SHALL be raised at region granularity with the changed bounds, so that dependent work —
/// procedural generation, derived fields, cached results — can be invalidated precisely."
///
/// A residency change is a distinct kind from a value change on purpose. A cache derived from a
/// region is invalid when the region's VALUES changed; it is merely less precise when a finer level
/// arrived. A consumer that treats the two alike either recomputes for nothing or misses an edit,
/// and M10's own PCG row is the first consumer that will care.
enum class FieldChangeKind : u8 {
    /// A producer published new values into the region.
    Values = 0,
    /// A tile became resident.
    Resident,
    /// A tile was evicted.
    Evicted,
};

[[nodiscard]] const char* field_change_kind_name(FieldChangeKind kind) noexcept;

struct FieldChange {
    FieldChangeKind kind = FieldChangeKind::Values;
    TileAddress address;
    FieldBounds bounds;
    /// The region's version AFTER the change. Monotonic per tile.
    u64 version = 0;
    /// Monotonic, store-wide. Two consumers reading at different points agree about order because
    /// they agree about this number — `world::CellEvent::sequence` is the same idea and this is
    /// deliberately the same shape, so a consumer of both does not learn two models.
    u64 sequence = 0;
};

/// The queue consumers take field changes from, in a declared order.
///
/// Modelled on `world::CellEventQueue` rather than invented: a subsystem that already consumes cell
/// events should not learn a second notification model to consume field events. Registration takes
/// an order key, `drain()` returns what that consumer has not seen, and `compact()` drops what
/// everyone has.
class FieldChangeQueue {
public:
    explicit FieldChangeQueue(Allocator& allocator) noexcept;

    using ConsumerId = u32;

    [[nodiscard]] Expected<ConsumerId, Error> add_consumer(const char* name, u32 order) noexcept;
    [[nodiscard]] Status emit(const FieldChange& change) noexcept;
    [[nodiscard]] Status drain(ConsumerId consumer, Array<FieldChange>& out) noexcept;
    void compact() noexcept;

    [[nodiscard]] usize pending() const noexcept { return changes_.size(); }
    [[nodiscard]] u64 next_sequence() const noexcept { return next_sequence_; }
    [[nodiscard]] usize consumer_count() const noexcept { return consumers_.size(); }

private:
    struct Consumer {
        ConsumerId id = 0;
        const char* name = "";
        u32 order = 0;
        u64 seen = 0;
    };

    Array<FieldChange> changes_;
    Array<Consumer> consumers_;
    u64 next_sequence_ = 1;
    u64 dropped_ = 0;
};

// --- The store
// ------------------------------------------------------------------------------------

class FieldWriter;

/// Sparse tiled storage for every declared field, and the sampling both halves of the engine do
/// through it.
///
/// It holds no producer logic and no streaming policy: `FieldWriter` is the only way in and
/// `FieldStreaming` (streaming.h) is what decides which tiles exist. The split is `residency`'s
/// "shared policy, separate storage" seen from the storage side.
class FieldStore {
public:
    /// `registry` and the partition configuration must outlive the store. The configuration is
    /// taken because a field is addressed by WORLD POSITION and the runtime's world position is
    /// cell-relative (`world::WorldPosition`); converting it is the partition's arithmetic and not
    /// a second copy of it.
    FieldStore(Allocator& allocator, const FieldRegistry& registry,
               const world::PartitionConfig& partition) noexcept;

    FieldStore(const FieldStore&) = delete;
    FieldStore& operator=(const FieldStore&) = delete;

    [[nodiscard]] const FieldRegistry& registry() const noexcept { return *registry_; }
    /// The allocator the store was built with. Handed out so that a consumer building something
    /// derived from the store — a GPU image, a list of tiles — allocates from the same domain the
    /// tiles do, and the memory is attributed where the pressure system will look for it.
    [[nodiscard]] Allocator& allocator() const noexcept { return *allocator_; }
    [[nodiscard]] const world::PartitionConfig& partition() const noexcept { return *partition_; }
    [[nodiscard]] FieldChangeQueue& changes() noexcept { return changes_; }

    /// A writer for the field the token names. The token is borrowed, not consumed: a producer
    /// writes many times over a session and holds its capability across all of them.
    [[nodiscard]] Expected<FieldWriter, Error> open_writer(const ProducerToken& token) noexcept;

    // --- Reading
    // ----------------------------------------------------------------------------------

    /// The finest resident value at a position. The presentation path; see the header note.
    [[nodiscard]] FieldSample sample(FieldId field, const world::WorldVec3d& at) const noexcept;
    /// The same, from the runtime's cell-relative position form.
    [[nodiscard]] FieldSample sample(FieldId field, const world::WorldPosition& at) const noexcept;

    /// The value at one declared level, whatever is resident elsewhere.
    [[nodiscard]] FieldSample sample_at(FieldId field, const world::WorldVec3d& at,
                                        FieldResidency level) const noexcept;

    /// The gameplay path: one declared level, the declared default where it has no data, and
    /// therefore an answer independent of streaming. M10 tasks.md 1.4.
    [[nodiscard]] FieldSample sample_deterministic(FieldId field,
                                                   const world::WorldVec3d& at) const noexcept;

    /// "WHEN a system samples a field at ten thousand positions, THEN it SHALL be able to do so in
    /// one batched call." One declaration lookup and one firewall decision for the whole batch
    /// rather than one per position, which is the per-sample overhead the requirement is about.
    [[nodiscard]] Status sample_many(FieldId field, Span<const world::WorldVec3d> positions,
                                     Span<FieldSample> out) const noexcept;
    [[nodiscard]] Status sample_many_deterministic(FieldId field,
                                                   Span<const world::WorldVec3d> positions,
                                                   Span<FieldSample> out) const noexcept;

    /// The diagnostic point query: the value, both layers, the version and the producer.
    [[nodiscard]] FieldPointQuery point_query(FieldId field,
                                              const world::WorldVec3d& at) const noexcept;

    // --- Residency
    // --------------------------------------------------------------------------------
    //
    // These are the streaming half. They take a token because putting cooked bytes into a field is
    // producing that field: "one producer per field" would be a rule about `stage()` alone if a
    // streamer could insert values for a field it does not own.

    /// Make a tile resident with cooked bytes. `data` is `tile_bytes(declaration)` long and in the
    /// store's own lattice order — see `tile_bytes()` and `lattice_offset()`.
    [[nodiscard]] Status insert_tile(const ProducerToken& token, const TileAddress& address,
                                     Span<const u8> data, bool guaranteed) noexcept;
    /// Make a tile resident holding the field's declared default everywhere. What a macro level
    /// that must exist for the whole world costs when nothing has been cooked for it yet.
    [[nodiscard]] Status insert_default_tile(const ProducerToken& token, const TileAddress& address,
                                             bool guaranteed) noexcept;
    /// Drop a tile. A guaranteed tile is refused: it is the coarse fallback other answers are
    /// defined in terms of.
    [[nodiscard]] Status evict_tile(const ProducerToken& token,
                                    const TileAddress& address) noexcept;

    [[nodiscard]] bool is_resident(const TileAddress& address) const noexcept;
    [[nodiscard]] u64 version_of(const TileAddress& address) const noexcept;
    [[nodiscard]] usize tile_count() const noexcept { return tiles_.size(); }
    [[nodiscard]] u64 bytes_resident() const noexcept { return bytes_; }

    /// The tiles of one field at one level, in address order. What the GPU image builder reads and
    /// what a visualisation iterates.
    [[nodiscard]] Status tiles_of(FieldId field, FieldResidency level,
                                  Array<TileAddress>& out) const noexcept;

    [[nodiscard]] FieldDiagnostics diagnostics(FieldId field) const noexcept;

    /// Raw tile bytes, for the GPU image builder and for a cooker writing them out. Empty for a
    /// tile that is not resident.
    [[nodiscard]] Span<const u8> tile_data(const TileAddress& address) const noexcept;

    // --- Potential and current state
    // ----------------------------------------------------------------

    /// Step a recovering field toward its potential. `environment-fields` — "Potential and current
    /// state": "Current state SHALL evolve toward potential over time at a declared rate".
    ///
    /// It is the substrate's step and not a producer's because the RATE is declared on the field,
    /// and a recovery each producer implemented would be a recovery with as many curves as
    /// producers. Reducing the current state is still the producer's — a fire writes the drop; this
    /// only closes the gap the fire left.
    [[nodiscard]] Status advance_recovery(const ProducerToken& token, f32 seconds) noexcept;

    [[nodiscard]] static u32 tile_bytes(const FieldDeclaration& declaration) noexcept;
    /// Byte offset of one lattice point within a tile. Vertical-major, then z, then x — so a
    /// horizontal row is contiguous, which is the order a sample reads two of.
    [[nodiscard]] static u32 lattice_offset(const FieldDeclaration& declaration, u32 x, u32 y,
                                            u32 z) noexcept;

private:
    friend class FieldWriter;

    struct Tile {
        TileAddress address;
        Array<u8> data;
        u64 version = 1;
        bool guaranteed = false;

        explicit Tile(Allocator& allocator) noexcept : data(allocator) {}
    };

    /// One level's answer, before the layers are combined.
    struct LayerSample {
        FieldValue value;
        bool resolved = false;
        u64 version = 0;
        u64 reads = 0;
    };

    [[nodiscard]] Tile* find_tile(const TileAddress& address) noexcept;
    [[nodiscard]] const Tile* find_tile(const TileAddress& address) const noexcept;
    [[nodiscard]] Status place_tile(const TileAddress& address, Span<const u8> data,
                                    bool guaranteed, FieldChangeKind kind) noexcept;
    [[nodiscard]] Status emit_change(FieldChangeKind kind, const FieldDeclaration& declaration,
                                     const TileAddress& address, u64 version) noexcept;
    /// One tile's step toward its potential. Split from `advance_recovery()` so that the loop over
    /// lattice points and the checks that let it run are separate things to read.
    [[nodiscard]] Status recover_tile(FieldWriter& writer, const FieldDeclaration& current,
                                      const FieldDeclaration& potential, const TileAddress& address,
                                      f32 fraction) noexcept;

    /// One lattice point of one tile, with a corner whose own tile is absent clamped into the tile
    /// that answered. Split out of `sample_layer()` because the missing-neighbour rule is the part
    /// a reader has to check against `gpu.cpp`'s transliteration of it, line by line.
    [[nodiscard]] FieldValue read_lattice_point(const FieldDeclaration& declaration,
                                                const TileAddress& centre_address,
                                                const Tile& centre, i64 gi, i64 gk,
                                                i64 gj) const noexcept;

    /// One level, one layer. The whole of the lattice arithmetic.
    [[nodiscard]] LayerSample sample_layer(const FieldDeclaration& declaration, FieldId field,
                                           u8 level, FieldLayer layer,
                                           const world::WorldVec3d& at) const noexcept;
    [[nodiscard]] FieldSample sample_level(const FieldDeclaration& declaration, FieldId field,
                                           u8 level, const world::WorldVec3d& at) const noexcept;

    Allocator* allocator_;
    const FieldRegistry* registry_;
    const world::PartitionConfig* partition_;

    Array<Tile> tiles_;
    HashMap<u64, usize> index_;
    FieldChangeQueue changes_;
    u64 bytes_ = 0;
    /// Sample cost, counted. Mutable because counting is not a change to what the store answers,
    /// and a diagnostic that forced every sampler to take a non-const store would be a diagnostic
    /// nobody leaves on.
    mutable HashMap<u64, u64> samples_;
    mutable HashMap<u64, u64> lattice_reads_;
};

/// The write side of one field, obtained from a `ProducerToken`.
///
/// **Writes are staged and become visible at `publish()`.** `environment-fields` — "Runtime field
/// modification": "writes SHALL be visible to consumers on a defined schedule rather than
/// immediately mid-frame". A consumer sampling during the frame a producer is writing therefore
/// sees the previous values everywhere rather than half of each, which is what makes a sample taken
/// by two systems in one frame the same sample.
class FieldWriter {
public:
    FieldWriter(const FieldWriter&) = delete;
    FieldWriter& operator=(const FieldWriter&) = delete;
    FieldWriter(FieldWriter&&) noexcept = default;
    FieldWriter& operator=(FieldWriter&&) noexcept = default;
    ~FieldWriter() = default;

    /// Begin editing one tile. Creates it if it is not resident, and copies the resident values in
    /// so that an edit of one lattice point does not erase the rest.
    [[nodiscard]] Status stage(const TileAddress& address) noexcept;

    /// Set one lattice point of a staged tile. `x` and `z` are in [0, kTileCells) and `y` in
    /// [0, vertical_cells).
    [[nodiscard]] Status set(const TileAddress& address, u32 x, u32 y, u32 z,
                             const FieldValue& value) noexcept;
    /// Set every lattice point of a staged tile.
    [[nodiscard]] Status fill(const TileAddress& address, const FieldValue& value) noexcept;

    /// Publish every staged tile: values become visible, versions increment, and one `Values`
    /// change event is raised per tile. The staging area is empty afterwards.
    [[nodiscard]] Status publish() noexcept;

    [[nodiscard]] usize staged() const noexcept { return staged_.size(); }
    [[nodiscard]] FieldId field() const noexcept { return field_; }

private:
    friend class FieldStore;

    struct Staged {
        TileAddress address;
        Array<u8> data;
        bool existed = false;

        explicit Staged(Allocator& allocator) noexcept : data(allocator) {}
    };

    FieldWriter(FieldStore& store, FieldId field, const FieldDeclaration& declaration) noexcept;

    [[nodiscard]] Staged* find_staged(const TileAddress& address) noexcept;

    FieldStore* store_;
    const FieldDeclaration* declaration_;
    FieldId field_;
    Array<Staged> staged_;
};

/// A consumer's handle on one field, with the firewall decided once.
///
/// `environment-fields` — "WHEN a field exists only to modulate a shader, THEN it SHALL be declared
/// visual, and gameplay SHALL be prevented from reading it by configuration validation." `open()`
/// is that prevention at the point of use, and `FieldRegistry::validate()` is the same rule applied
/// to the whole configuration before a frame runs. Both call `determinism::may_read()`; neither
/// adds a rule of its own.
///
/// **WHICH SAMPLING PATH A READER GETS IS DECIDED BY THE FIELD, NOT BY THE READER.** A
/// gameplay-visible field is sampled deterministically for everyone, including the renderer.
/// Dispatching on the reader's class instead would make "terrain material, foliage placement and
/// audio all sample the same field and observe the same value" false for exactly the fields where
/// it matters most — one of them would be reading the finest resident level and the others the
/// declared one.
class FieldReader {
public:
    [[nodiscard]] static Expected<FieldReader, Error> open(
        const FieldStore& store, FieldId field, determinism::SimulationClass reader) noexcept;

    [[nodiscard]] FieldSample sample(const world::WorldVec3d& at) const noexcept;
    [[nodiscard]] FieldSample sample(const world::WorldPosition& at) const noexcept;
    [[nodiscard]] Status sample_many(Span<const world::WorldVec3d> positions,
                                     Span<FieldSample> out) const noexcept;

    [[nodiscard]] FieldId field() const noexcept { return field_; }
    [[nodiscard]] const FieldDeclaration& declaration() const noexcept { return *declaration_; }
    [[nodiscard]] bool deterministic() const noexcept { return deterministic_; }

private:
    FieldReader(const FieldStore& store, FieldId field, const FieldDeclaration& declaration,
                bool deterministic) noexcept
        : store_(&store),
          declaration_(&declaration),
          field_(field),
          deterministic_(deterministic) {}

    const FieldStore* store_;
    const FieldDeclaration* declaration_;
    FieldId field_;
    bool deterministic_ = false;
};

// --- Encoding
// ---------------------------------------------------------------------------------------
//
// Free functions rather than members, because the GPU image builder and a cooker both encode
// without a store, and because the decode below is the exact arithmetic `gpu.h`'s image sampler
// transliterates — one function to compare against rather than a rule to re-derive.

void encode_value(const FieldDeclaration& declaration, const FieldValue& value,
                  u8* destination) noexcept;
[[nodiscard]] FieldValue decode_value(const FieldDeclaration& declaration,
                                      const u8* source) noexcept;

}  // namespace cy::environment
