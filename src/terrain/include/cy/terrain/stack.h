#pragma once
// The terrain modifier stack: a generator plus an ordered, non-destructive stack of modifiers, and
// the cook that flattens it. M10 task 2.2.
//
// `terrain` — "Terrain modifier stack": authoring "SHALL be non-destructive: a terrain SHALL be
// defined as a generator plus an ordered stack of modifiers — noise, erosion, spline roads, river
// carving, area flattening, craters, and artist sculpting — each of which can be REORDERED,
// DISABLED, OR EDITED AFTER LATER ONES EXIST", writing "stamps and layers into the stack rather
// than destructively rewriting the source heightfield". "Cooking SHALL flatten the stack; the
// runtime SHALL NOT carry it."
//
// ================================================================================================
// WHAT MAKES REORDERING WORK, AND WHY IT IS NOT A FEATURE OF THE EVALUATOR
// ================================================================================================
//
// "WHEN an author inserts an erosion pass beneath an existing sculpt, THEN the sculpt SHALL be
// preserved and reapplied above it."
//
// That scenario is satisfied by the STORAGE, not by the evaluation order. A sculpt that had been
// applied by editing the heightfield would be indistinguishable from the terrain by the time the
// erosion arrived, and no evaluator could lift it back out. So a sculpt is a STAMP — its own
// buffer of offsets, owned by the modifier — and evaluation is a fold over the stack from the
// generator upward. Reordering is then a permutation of a list, and every modifier survives it.
//
// ================================================================================================
// THE STACK IS A PROCEDURAL PROGRAM, AND THE FOUR CONDITIONS THE M10 SPIKE IMPOSED APPLY HERE
// ================================================================================================
//
// "Generators and modifiers SHALL BE PROCEDURAL PROGRAMS (see `procedural-content-generation`):
// they are authored as graphs, compiled to programs, executed by region, cached by derivation key,
// and invalidated by the dependency and radius declarations that capability defines. Terrain SHALL
// NOT maintain a separate procedural execution model."
//
// `src/pcg/` does not exist yet, so this file is shaped to be lowered into it rather than to
// replace it, and it obeys the conditions M10's spike made binding (design.md §1.6):
//
//   * RANDOMNESS IS `cy::determinism::RandomStream`, substream then draw, keyed by stable
//     identifiers — the modifier's index in the stack and the lattice point — never by a traversal
//     counter. Condition 4.
//   * EVERY MODIFIER DECLARES ITS REACH (`Modifier::radius`) and evaluation reads nothing outside
//     its own tile plus that reach: `evaluate()` builds a PADDED grid of exactly the declared halo
//     and crops. An erosion pass with a halo smaller than its iteration count would produce a tile
//     that depends on where the tile boundaries are, which is the seam version of condition 2.
//   * AN ITERATIVE MODIFIER DECLARES ITS ITERATIONS AND RUNS ALL OF THEM. There is no sweep budget
//     here, because a budgeted result is not cacheable — condition 3, and the spike's only axis
//     with no survivor anywhere.
//   * EVALUATION IS A PURE FUNCTION OF THE STACK AND THE TILE. It reads no sibling tile's OUTPUT.
//     Condition 2.
//
// `derivation_key()` is what a cache and an incremental re-cook key on, and it covers every enabled
// modifier's parameters in order — so disabling one, moving one, or editing one changes it.

#include <cy/core/base/expected.h>
#include <cy/core/determinism/random.h>
#include <cy/core/memory/array.h>
#include <cy/terrain/tile.h>

namespace cy::terrain {

/// The modifiers `terrain` names, in the order it names them, plus the two the specification's
/// other requirements need: a hole cutter and a decoration layer writer.
enum class ModifierKind : u8 {
    /// Fractal value noise added to the surface.
    Noise = 0,
    /// Thermal smoothing, `iterations` passes. Its declared reach must cover its iterations.
    Erosion,
    /// A spline road: flattens toward the spline and writes its decoration layer.
    RoadSpline,
    /// A spline river: carves below the surrounding surface.
    RiverCarve,
    /// Levels a region toward `height`.
    Flatten,
    /// A radial depression or mound.
    Crater,
    /// An artist's stamp: its own buffer of offsets, added over whatever is beneath it.
    Sculpt,
    /// Cuts the surface away. A cave mouth, a pit, a building's interior footprint.
    Hole,
};

[[nodiscard]] const char* modifier_kind_name(ModifierKind kind) noexcept;

/// The base surface the stack is folded over.
struct TerrainGenerator {
    /// Metres between noise features at the coarsest octave.
    f32 period = 512.0F;
    /// Metres of relief the coarsest octave contributes.
    f32 amplitude = 120.0F;
    /// How many octaves, each half the period and half the amplitude of the one before.
    u32 octaves = 5;
    /// The surface the noise is added to.
    f32 base_height = 0.0F;
};

/// One modifier. A value type: the stack owns its spline points and its sculpt samples in its own
/// pools, so reordering is a permutation of this array and nothing has to be re-linked.
struct Modifier {
    ModifierKind kind = ModifierKind::Noise;
    const char* name = "";
    bool enabled = true;

    /// What the modifier's own authored data touches.
    TerrainBounds bounds;
    /// How far beyond `bounds` its effect reaches, in metres. THE DECLARED RADIUS: `dirty_tiles()`
    /// is exactly bounds-plus-this, and an evaluation reads nothing outside it.
    f32 radius = 0.0F;

    /// Noise, Crater, Sculpt: metres of change at full weight. Negative digs.
    f32 amplitude = 0.0F;
    /// Noise: metres between features. RoadSpline, RiverCarve: the width of the corridor.
    f32 period = 64.0F;
    /// Erosion: how many passes. Noise: how many octaves.
    u32 iterations = 1;
    /// Flatten, RoadSpline: the height to level toward. RiverCarve: how far below the surrounding
    /// surface to carve.
    f32 height = 0.0F;
    /// The material layer this modifier writes, where it writes one.
    u8 layer = 0;
    /// Whether content placed by other systems is suppressed over this modifier's footprint. A
    /// road's answer is yes: "the road SHALL blend into the terrain material and SUPPRESS FOLIAGE
    /// placement along its width."
    bool suppresses_foliage = false;

    /// Spline control points, as a range in the stack's point pool. Set by `add_spline()`.
    u32 first_point = 0;
    u32 point_count = 0;
    /// Sculpt samples, as a range in the stack's sample pool, over a `stamp_edge` x `stamp_edge`
    /// grid spanning `bounds`. Set by `add_sculpt()`.
    u32 first_sample = 0;
    u32 stamp_edge = 0;
};

/// What one modifier's edit reaches, as a rectangle. `terrain` — "WHEN a road spline is moved, THEN
/// only the terrain regions its stamp and declared radius reach SHALL be re-evaluated."
[[nodiscard]] TerrainBounds modifier_reach(const Modifier& modifier) noexcept;

/// The generator, the modifiers, their pools, and the evaluation.
class ModifierStack {
public:
    ModifierStack(Allocator& allocator, const TileLayout& layout, u64 seed) noexcept;

    ModifierStack(const ModifierStack&) = delete;
    ModifierStack& operator=(const ModifierStack&) = delete;

    void set_generator(const TerrainGenerator& generator) noexcept { generator_ = generator; }
    [[nodiscard]] const TerrainGenerator& generator() const noexcept { return generator_; }

    /// Append a modifier. Returns its index.
    [[nodiscard]] Expected<u32, Error> add(const Modifier& modifier) noexcept;
    /// Insert one BENEATH an existing modifier. The scenario's own operation.
    [[nodiscard]] Status insert_at(u32 index, const Modifier& modifier) noexcept;
    /// Move a modifier to another position in the order.
    [[nodiscard]] Status move(u32 from, u32 to) noexcept;
    [[nodiscard]] Status set_enabled(u32 index, bool enabled) noexcept;
    /// Edit a modifier in place, keeping its pool ranges. What moving a road spline's parameters
    /// does; `add_spline()` is what moving its points does.
    [[nodiscard]] Status edit(u32 index, const Modifier& modifier) noexcept;

    /// Attach a polyline to a modifier, in absolute world coordinates.
    [[nodiscard]] Status add_spline(u32 index, Span<const TerrainPoint> points) noexcept;
    /// Attach a sculpt stamp: `edge * edge` offsets in metres, spanning the modifier's bounds.
    [[nodiscard]] Status add_sculpt(u32 index, u32 edge, Span<const f32> offsets) noexcept;

    /// Declare that a material layer suppresses other systems' placement. A declared mapping, so a
    /// foliage row asks a question rather than special-casing a road.
    [[nodiscard]] Status declare_decoration(u8 layer, bool suppresses_foliage) noexcept;
    [[nodiscard]] bool suppresses_foliage(u8 layer) const noexcept;

    [[nodiscard]] Span<const Modifier> modifiers() const noexcept { return modifiers_.span(); }
    [[nodiscard]] usize size() const noexcept { return modifiers_.size(); }
    [[nodiscard]] u64 seed() const noexcept { return seed_; }

    /// Evaluate one tile. A pure function of the stack, the layout and the tile coordinate: no
    /// sibling tile's output is read and no state survives the call.
    [[nodiscard]] Expected<TerrainTile, Error> evaluate(Allocator& allocator,
                                                        const TileCoord& coord) const noexcept;

    /// Everything a tile's content is a function of, hashed. A cook cache and an incremental
    /// re-cook key on it, and `TerrainTile::derivation_key` carries it forward.
    [[nodiscard]] u64 derivation_key(const TileCoord& coord) const noexcept;

    /// The tiles one modifier's edit reaches at a level: its bounds expanded by its declared
    /// radius, and nothing else.
    [[nodiscard]] Status dirty_tiles(u32 index, u8 level, Array<TileCoord>& out) const noexcept;

    /// Cook a rectangle of tiles into a store. `terrain` — "Cooking SHALL flatten the stack; the
    /// runtime SHALL NOT carry it", and the runtime type this fills (`TerrainStore`) has no way to
    /// name a stack, which is the statement made structural.
    [[nodiscard]] Status flatten(TerrainStore& out, i32 min_tile_x, i32 min_tile_z, i32 max_tile_x,
                                 i32 max_tile_z, u8 level) const noexcept;

private:
    /// A padded height grid: the tile plus the halo the enabled modifiers declared.
    struct Padded {
        Array<f32> heights;
        u32 edge = 0;
        u32 pad = 0;

        explicit Padded(Allocator& allocator) noexcept : heights(allocator) {}

        [[nodiscard]] f32& at(u32 i, u32 j) noexcept { return heights[(j * edge) + i]; }
        [[nodiscard]] f32 at(u32 i, u32 j) const noexcept { return heights[(j * edge) + i]; }
    };

    [[nodiscard]] u32 halo_samples(const TileCoord& coord) const noexcept;
    [[nodiscard]] Expected<Padded, Error> generate(Allocator& allocator, const TileCoord& coord,
                                                   u32 pad) const noexcept;
    void apply(const Modifier& modifier, u32 index, const TileCoord& coord,
               Padded& grid) const noexcept;
    /// The modifier's contribution at one position, in metres, and the weight it carries there.
    [[nodiscard]] f32 contribution(const Modifier& modifier, u32 index, f64 x, f64 z,
                                   f32 current) const noexcept;
    void write_material(const Modifier& modifier, const TileCoord& coord,
                        TerrainTile& tile) const noexcept;

    Allocator* allocator_;
    TileLayout layout_;
    u64 seed_;
    TerrainGenerator generator_;
    Array<Modifier> modifiers_;
    Array<TerrainPoint> points_;
    Array<f32> samples_;
    Array<u16> decorations_;  ///< layer in the low byte, suppression flag in bit 8
};

}  // namespace cy::terrain
