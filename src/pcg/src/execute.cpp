// The evaluator. See include/cy/pcg/execute.h.
//
// The file is ordered the way a region's evaluation is: the pure helpers, then one function per
// node kind, then the stage driver, then the fixed point and the budgeted step. Each `eval_*`
// function is a pure function of the region's inputs and of its neighbours' PREVIOUS-STAGE output,
// which is the property the whole of design.md §1.2 turns on.

#include <cy/pcg/execute.h>

#include <cy/environment/field.h>
#include <cy/environment/store.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace cy::pcg {

namespace {

using determinism::RandomStream;

[[nodiscard]] f32 smooth_step(f32 t) noexcept {
    return t * t * (3.0F - 2.0F * t);
}

/// A lattice value from a counter-based stream. Offset so that a negative lattice coordinate in a
/// halo is still a distinct subject rather than aliasing a positive one.
[[nodiscard]] f32 lattice_value(const RandomStream& stream, i32 lx, i32 lz) noexcept {
    return stream.unit_float(generation_point(), static_cast<u64>(lx + (1 << 20)),
                             static_cast<u64>(lz + (1 << 20)));
}

/// Value noise at absolute world coordinates. A pure function of position, so two regions agree on
/// their shared boundary by construction rather than by a seam-fixing pass — which is the seam bug
/// that would otherwise dominate every digest comparison in this module.
[[nodiscard]] f32 value_noise(const RandomStream& stream, f64 x, f64 z) noexcept {
    const f64 fx = std::floor(x);
    const f64 fz = std::floor(z);
    const i32 lx = static_cast<i32>(fx);
    const i32 lz = static_cast<i32>(fz);
    const f32 tx = smooth_step(static_cast<f32>(x - fx));
    const f32 tz = smooth_step(static_cast<f32>(z - fz));
    const f32 v00 = lattice_value(stream, lx, lz);
    const f32 v10 = lattice_value(stream, lx + 1, lz);
    const f32 v01 = lattice_value(stream, lx, lz + 1);
    const f32 v11 = lattice_value(stream, lx + 1, lz + 1);
    const f32 top = v00 + (v10 - v00) * tx;
    const f32 bottom = v01 + (v11 - v01) * tx;
    return top + (bottom - top) * tz;
}

[[nodiscard]] u32 clamp_cell(i32 value) noexcept {
    if (value < 0) {
        return 0;
    }
    if (value >= static_cast<i32>(kRegionCells)) {
        return kRegionCells - 1;
    }
    return static_cast<u32>(value);
}

/// The mean of a raster channel. The region's macro summary, and deliberately a projection of the
/// SAME values the detail was generated from — which is what makes "materialisation is consistent
/// with macro state" true by construction rather than by a reconciliation pass.
[[nodiscard]] f32 channel_mean(const Raster& raster, AttributeId channel) noexcept {
    const Span<const f32> values = raster.values(channel);
    if (values.empty()) {
        return 0.0F;
    }
    f64 total = 0.0;
    for (f32 value : values) {
        total += static_cast<f64>(value);
    }
    return static_cast<f32>(total / static_cast<f64>(values.size()));
}

}  // namespace

const char* run_outcome_name(RunOutcome outcome) noexcept {
    switch (outcome) {
        case RunOutcome::Complete:
            return "complete";
        case RunOutcome::BudgetExhausted:
            return "budget-exhausted";
        case RunOutcome::Cancelled:
            return "cancelled";
    }
    return "unknown";
}

// --- FlatSpatialQuery -------------------------------------------------------------------------

void FlatSpatialQuery::height_batch(Span<const f64> x, Span<const f64> z,
                                    Span<f32> out) const noexcept {
    (void)x;
    (void)z;
    for (f32& value : out) {
        value = height_;
    }
}

void FlatSpatialQuery::slope_batch(Span<const f64> x, Span<const f64> z,
                                   Span<f32> out) const noexcept {
    (void)x;
    (void)z;
    for (f32& value : out) {
        value = 0.0F;
    }
}

void FlatSpatialQuery::surface_batch(Span<const f64> x, Span<const f64> z,
                                     Span<u8> out) const noexcept {
    (void)x;
    (void)z;
    for (u8& value : out) {
        value = 1;
    }
}

// --- RegionState and the world ------------------------------------------------------------------

u64 RegionState::bytes() const noexcept {
    return raster.bytes() + candidates.bytes() + accepted.bytes() + provenance.bytes() +
           sizeof(outflow) + stage_digests.capacity() * sizeof(u64);
}

namespace {

[[nodiscard]] u64 world_key(const RegionCoord& region) noexcept {
    const u64 x = static_cast<u64>(static_cast<u32>(region.x));
    const u64 z = static_cast<u64>(static_cast<u32>(region.z));
    return (x << 40U) | ((z & 0xFFFFFFFFULL) << 8U) | static_cast<u64>(region.level);
}

}  // namespace

RegionState* GenerationWorld::find(const RegionCoord& region) noexcept {
    const usize* slot = index_.find(world_key(region));
    return slot != nullptr ? &states_[*slot] : nullptr;
}

const RegionState* GenerationWorld::find(const RegionCoord& region) const noexcept {
    const usize* slot = index_.find(world_key(region));
    return slot != nullptr ? &states_[*slot] : nullptr;
}

Expected<RegionState*, Error> GenerationWorld::ensure(const RegionCoord& region) noexcept {
    if (RegionState* existing = find(region); existing != nullptr) {
        return existing;
    }
    if (!extent_.contains(region)) {
        return make_unexpected(
            Error{ErrorCode::OutOfRange, "pcg: a region outside the generator's declared extent"});
    }
    Expected<RegionState*, Error> slot = states_.emplace_back(*allocator_);
    if (!slot) {
        return slot;
    }
    (*slot)->coord = region;
    const usize index = states_.size() - 1;
    if (Expected<usize*, Error> inserted = index_.insert(world_key(region), index); !inserted) {
        return make_unexpected(inserted.error());
    }
    // `states_` relocates on growth, so the pointer is taken after the index is written rather than
    // held across it.
    return &states_[index];
}

Status GenerationWorld::demote(const RegionCoord& region) noexcept {
    RegionState* state = find(region);
    if (state == nullptr) {
        return ok();
    }
    // The macro summary is updated FROM the detail before the detail goes, which is the
    // specification's "Demotion SHALL update macro state from the detailed representation, so the
    // round trip does not lose what happened." A demotion that dropped the points and left a macro
    // value computed before anything happened in the region would lose exactly that.
    state->macro_instances = static_cast<u32>(state->accepted.size());
    state->accepted.clear();
    state->candidates.clear();
    state->provenance.clear();
    return ok();
}

u64 GenerationWorld::bytes() const noexcept {
    u64 total = 0;
    for (const RegionState& state : states_) {
        total += state.bytes();
    }
    return total;
}

bool GenerationWorld::complete_through(u8 stage) const noexcept {
    if (static_cast<i64>(states_.size()) != extent_.count()) {
        return false;
    }
    for (const RegionState& state : states_) {
        for (u8 index = 0; index <= stage; ++index) {
            if (!state.has_stage(index)) {
                return false;
            }
        }
    }
    return true;
}

// --- Generator: construction ------------------------------------------------------------------

Generator::Generator(Allocator& allocator, Program program, GenerationWorld& world) noexcept
    : allocator_(&allocator),
      program_(std::move(program)),
      world_(&world),
      dirty_(allocator),
      changed_(allocator),
      sweep_changed_set_(allocator),
      query_x_(allocator),
      query_z_(allocator),
      query_f_(allocator),
      query_g_(allocator),
      query_flags_(allocator),
      keep_(allocator),
      cursor_members_(allocator),
      ledger_(allocator),
      reads_(allocator),
      profile_(allocator) {}

Expected<Generator, Error> Generator::create(Allocator& allocator, Program program,
                                             GenerationWorld& world) noexcept {
    const usize stages = program.stages().size();
    Generator generator(allocator, std::move(program), world);
    const RegionExtent& extent = world.extent();
    for (usize index = 0; index < stages; ++index) {
        RegionSet dirty(allocator, extent);
        if (Status sized = dirty.resize(); !sized) {
            return make_unexpected(sized.error());
        }
        RegionSet changed(allocator, extent);
        if (Status sized = changed.resize(); !sized) {
            return make_unexpected(sized.error());
        }
        RegionSet swept(allocator, extent);
        if (Status sized = swept.resize(); !sized) {
            return make_unexpected(sized.error());
        }
        if (Status pushed = generator.dirty_.push_back(std::move(dirty)); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (Status pushed = generator.changed_.push_back(std::move(changed)); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (Status pushed = generator.sweep_changed_set_.push_back(std::move(swept)); !pushed) {
            return make_unexpected(pushed.error());
        }
        StageProfile profile;
        profile.name = generator.program_.stages()[index].name;
        profile.stage = static_cast<u8>(index);
        if (Status pushed = generator.profile_.stages.push_back(profile); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    // One allocation each, for the whole life of the generator. A region's evaluation allocates
    // nothing after this, which is what "no per-point heap allocation" needs of the scratch too.
    // The batched spatial query carries one entry per CANDIDATE, so the scratch is sized by the
    // largest scatter in the program as well as by the raster.
    usize scratch = kRegionCellCount;
    for (const Stage& stage : generator.program_.stages()) {
        if (stage.kind == NodeKind::Scatter && stage.params.count > scratch) {
            scratch = stage.params.count;
        }
    }
    if (Status sized = generator.query_x_.resize(scratch); !sized) {
        return make_unexpected(sized.error());
    }
    if (Status sized = generator.query_z_.resize(scratch); !sized) {
        return make_unexpected(sized.error());
    }
    if (Status sized = generator.query_f_.resize(scratch); !sized) {
        return make_unexpected(sized.error());
    }
    if (Status sized = generator.query_flags_.resize(scratch); !sized) {
        return make_unexpected(sized.error());
    }
    return generator;
}

RegionCoord Generator::region_of(const GenerationContext& context, f64 x, f64 z) const noexcept {
    const f64 size = program_.region_metres();
    const f64 local_x = (x - context.origin_x) / size;
    const f64 local_z = (z - context.origin_z) / size;
    return RegionCoord{static_cast<i32>(std::floor(local_x)), static_cast<i32>(std::floor(local_z)),
                       program_.region_level()};
}

void Generator::region_origin(const GenerationContext& context, const RegionCoord& region, f64& x,
                              f64& z) const noexcept {
    x = context.origin_x + static_cast<f64>(region.x) * program_.region_metres();
    z = context.origin_z + static_cast<f64>(region.z) * program_.region_metres();
}

const RegionSet* Generator::changed_at(u8 stage) const noexcept {
    return stage < changed_.size() ? &changed_[stage] : nullptr;
}

// --- Per-node evaluation ----------------------------------------------------------------------

namespace {

/// Everything one stage's evaluation of one region is given. A struct rather than eleven arguments,
/// and const where it can be: the only mutable things are the region's own state and the scratch.
struct EvalArgs {
    const Program& program;
    const GenerationContext& context;
    const Stage& stage;
    u8 stage_index;
    RegionCoord region;
    f64 origin_x;
    f64 origin_z;
    f64 cell_metres;
    u64 seed;
    GenerationWorld& world;
    ReadLedger& reads;
    Array<f64>& query_x;
    Array<f64>& query_z;
    Array<f32>& query_f;
    Array<f32>& query_g;
    Array<u8>& query_flags;
    Array<u8>& keep;
};

[[nodiscard]] AttributeId input_channel(const EvalArgs& args, u8 slot) noexcept {
    if (slot >= args.stage.input_count) {
        return AttributeId{};
    }
    return args.program.stages()[args.stage.inputs[slot]].output;
}

void eval_constant(const EvalArgs& args, RegionState& state) noexcept {
    const Span<f32> values = state.raster.values_mutable(args.stage.output);
    for (f32& value : values) {
        value = args.stage.params.value;
    }
}

void eval_noise(const EvalArgs& args, RegionState& state) noexcept {
    const Span<f32> values = state.raster.values_mutable(args.stage.output);
    if (values.empty()) {
        return;
    }
    const determinism::RandomSource source(args.seed);
    const u32 octaves = args.stage.params.count == 0 ? 1U : args.stage.params.count;
    for (u32 cz = 0; cz < kRegionCells; ++cz) {
        for (u32 cx = 0; cx < kRegionCells; ++cx) {
            const f64 wx = args.origin_x + (static_cast<f64>(cx) + 0.5) * args.cell_metres;
            const f64 wz = args.origin_z + (static_cast<f64>(cz) + 0.5) * args.cell_metres;
            f32 total = 0.0F;
            f32 amplitude = args.stage.params.amplitude;
            f64 frequency = static_cast<f64>(args.stage.params.frequency);
            for (u64 octave = 0; octave < octaves; ++octave) {
                const RandomStream stream = source.stream(determinism::substream(
                    determinism::StreamId{args.stage.identity.value}, octave));
                total += amplitude * value_noise(stream, wx * frequency, wz * frequency);
                amplitude *= 0.5F;
                frequency *= 2.0;
            }
            values[static_cast<usize>(cz) * kRegionCells + cx] = total;
        }
    }
}

[[nodiscard]] Status eval_field_read(const EvalArgs& args, RegionState& state) noexcept {
    const Span<f32> values = state.raster.values_mutable(args.stage.output);
    if (values.empty()) {
        return ok();
    }
    if (args.stage.query_fused) {
        // The compiler found an earlier read of the SAME field and made it this stage's input. One
        // sampling, two channels — the specification's "spatial query fusion", and it saves the
        // work rather than only reporting it.
        const Span<const f32> source = state.raster.values(input_channel(args, 0));
        for (usize index = 0; index < values.size() && index < source.size(); ++index) {
            values[index] = source[index];
        }
        return ok();
    }
    const environment::FieldId field{args.stage.params.field};
    for (u32 cz = 0; cz < kRegionCells; ++cz) {
        for (u32 cx = 0; cx < kRegionCells; ++cx) {
            const usize at = static_cast<usize>(cz) * kRegionCells + cx;
            if (args.context.fields == nullptr) {
                // No store bound: the value is the field's declared default everywhere, which is
                // exactly what the substrate returns for a region nothing has produced. A generator
                // running in a cook with no field data must produce the same thing it would with an
                // empty store, or the cook and the runtime would disagree about an absent field.
                values[at] = 0.0F;
                continue;
            }
            const world::WorldVec3d position{
                args.origin_x + (static_cast<f64>(cx) + 0.5) * args.cell_metres, 0.0,
                args.origin_z + (static_cast<f64>(cz) + 0.5) * args.cell_metres};
            // The DETERMINISTIC path, always. A generated world is authoritative state — a save, a
            // replay and a peer all have to agree on it — and `sample()` walks levels finest-first,
            // so its answer depends on what streamed. `environment`'s own rule is that which path a
            // reader gets is decided by the FIELD; what this module decides is that it never asks
            // for the streaming-dependent one.
            const environment::FieldSample sample =
                args.context.fields->sample_deterministic(field, position);
            values[at] = sample.value.x();
        }
    }
    return ok();
}

void eval_stamp(const EvalArgs& args, RegionState& state) noexcept {
    const AttributeId source = input_channel(args, 0);
    const Span<const f32> input = state.raster.values(source);
    const Span<f32> values = state.raster.values_mutable(args.stage.output);
    if (values.empty()) {
        return;
    }
    for (u32 cz = 0; cz < kRegionCells; ++cz) {
        for (u32 cx = 0; cx < kRegionCells; ++cx) {
            const usize at = static_cast<usize>(cz) * kRegionCells + cx;
            const f64 wx = args.origin_x + (static_cast<f64>(cx) + 0.5) * args.cell_metres;
            const f64 wz = args.origin_z + (static_cast<f64>(cz) + 0.5) * args.cell_metres;
            f32 total = at < input.size() ? input[at] : 0.0F;
            for (const AuthoredStamp& stamp : args.context.stamps) {
                if (!(stamp.node == args.stage.identity) || stamp.radius <= 0.0) {
                    continue;
                }
                const f64 dx = wx - stamp.x;
                const f64 dz = wz - stamp.z;
                const f64 distance = std::sqrt(dx * dx + dz * dz);
                if (distance >= stamp.radius) {
                    continue;
                }
                const f32 falloff = 1.0F - static_cast<f32>(distance / stamp.radius);
                total += stamp.amount * falloff * falloff;
            }
            values[at] = total;
        }
    }
}

/// The value of `channel` at a world cell, reading a NEIGHBOUR's region where the cell falls
/// outside this one. Clamped at the extent's edge, which is the same convention the halo uses.
[[nodiscard]] f32 sample_world_cell(const EvalArgs& args, AttributeId channel, i32 gx,
                                    i32 gz) noexcept {
    const i32 cells = static_cast<i32>(kRegionCells);
    i32 rx = args.region.x + (gx >= 0 ? gx / cells : (gx - cells + 1) / cells);
    i32 rz = args.region.z + (gz >= 0 ? gz / cells : (gz - cells + 1) / cells);
    i32 lx = gx - (rx - args.region.x) * cells;
    i32 lz = gz - (rz - args.region.z) * cells;
    const RegionCoord neighbour{rx, rz, args.region.level};
    const RegionState* source = args.world.find(neighbour);
    if (source == nullptr) {
        // Outside the generated world: clamp to this region's own edge, so a halo at the world's
        // boundary is the boundary value repeated rather than a zero that would look like a cliff.
        source = args.world.find(args.region);
        lx = static_cast<i32>(clamp_cell(gx));
        lz = static_cast<i32>(clamp_cell(gz));
    }
    if (source == nullptr) {
        return 0.0F;
    }
    return source->raster.at(channel, clamp_cell(lx), clamp_cell(lz));
}

/// A bounded gather: `iteration_bound` relaxation passes over a halo of `reach_regions` regions.
///
/// The halo makes the region's own cells EXACT — identical to what a whole-world pass would have
/// produced — as long as the halo in CELLS is at least the pass count, which the compiler cannot
/// check because the halo is declared in regions. `kRegionCells` is 16 and a relaxation with more
/// than 16 passes is a `Propagate` wearing the wrong name; the suite covers the equality rather
/// than assuming it.
[[nodiscard]] Status eval_smooth(const EvalArgs& args, RegionState& state) noexcept {
    const AttributeId source = input_channel(args, 0);
    // THE HALO IS THE PASS COUNT, not the declared reach. K relaxation passes shrink the useful
    // window by one cell each, so K cells of halo make this region's own cells EXACT — identical to
    // what a whole-world pass would have produced. The declared reach is the ceiling the compiler
    // checked (`HaloTooSmallForIteration`); reading less than it is what lets the read ledger
    // NARROW the next invalidation, which is design.md §1.3's answer to the long-range gather.
    const i32 halo = static_cast<i32>(args.stage.iteration_bound);
    const i32 halo_regions =
        static_cast<i32>((args.stage.iteration_bound + kRegionCells - 1) / kRegionCells);
    const i32 span = static_cast<i32>(kRegionCells) + 2 * halo;
    Array<f32>& window = args.query_f;
    Array<f32>& next = args.query_g;
    if (Status sized = window.resize(static_cast<usize>(span) * static_cast<usize>(span)); !sized) {
        return sized;
    }
    if (Status sized = next.resize(window.size()); !sized) {
        return sized;
    }
    for (i32 wz = 0; wz < span; ++wz) {
        for (i32 wx = 0; wx < span; ++wx) {
            window[static_cast<usize>(wz) * static_cast<usize>(span) + static_cast<usize>(wx)] =
                sample_world_cell(args, source, wx - halo, wz - halo);
        }
    }
    for (i32 dz = -halo_regions; dz <= halo_regions; ++dz) {
        for (i32 dx = -halo_regions; dx <= halo_regions; ++dx) {
            if (Status recorded = args.reads.read(
                    RegionCoord{args.region.x + dx, args.region.z + dz, args.region.level});
                !recorded) {
                return recorded;
            }
        }
    }
    const f32 rate = args.stage.params.amplitude;
    for (u32 pass = 0; pass < args.stage.iteration_bound; ++pass) {
        for (usize index = 0; index < window.size(); ++index) {
            next[index] = window[index];
        }
        for (i32 wz = 1; wz < span - 1; ++wz) {
            for (i32 wx = 1; wx < span - 1; ++wx) {
                const usize at =
                    static_cast<usize>(wz) * static_cast<usize>(span) + static_cast<usize>(wx);
                const f32 neighbours = window[at - 1] + window[at + 1] +
                                       window[at - static_cast<usize>(span)] +
                                       window[at + static_cast<usize>(span)];
                next[at] = window[at] + rate * (0.25F * neighbours - window[at]);
            }
        }
        for (usize index = 0; index < window.size(); ++index) {
            window[index] = next[index];
        }
    }
    const Span<f32> values = state.raster.values_mutable(args.stage.output);
    for (u32 cz = 0; cz < kRegionCells; ++cz) {
        for (u32 cx = 0; cx < kRegionCells; ++cx) {
            values[static_cast<usize>(cz) * kRegionCells + cx] =
                window[static_cast<usize>(static_cast<i32>(cz) + halo) * static_cast<usize>(span) +
                       static_cast<usize>(static_cast<i32>(cx) + halo)];
        }
    }
    return ok();
}

/// Water arriving from the four neighbours, at the boundary cells it arrives on.
void gather_inflow(const EvalArgs& args, f32* water) noexcept {
    for (u32 side = 0; side < kRegionSides; ++side) {
        const RegionCoord neighbour{args.region.x + kSideDx[side], args.region.z + kSideDz[side],
                                    args.region.level};
        const RegionState* source = args.world.find(neighbour);
        if (source == nullptr) {
            continue;
        }
        const u32 opposite = (side + 2) % kRegionSides;
        for (u32 pos = 0; pos < kRegionCells; ++pos) {
            const f32 delivered = source->outflow[opposite * kRegionCells + pos];
            if (delivered <= 0.0F) {
                continue;
            }
            u32 cx = pos;
            u32 cz = pos;
            if (side == 0) {
                cz = 0;
            }
            if (side == 2) {
                cz = kRegionCells - 1;
            }
            if (side == 1) {
                cx = kRegionCells - 1;
            }
            if (side == 3) {
                cx = 0;
            }
            water[static_cast<usize>(cz) * kRegionCells + cx] += delivered;
        }
    }
}

/// THE TRANSITIVE EDGE. One unit of rain per cell routes to its lowest four-neighbour; water that
/// leaves the region is deposited in `outflow`, water that arrives is seeded at the boundary first.
///
/// A region READS exactly four neighbours, so the declared reach is one and every implementation
/// would declare it so. The REACHABLE SET is not one: water crosses a boundary and keeps going, so
/// the region whose rivers an edit moves may be any distance downhill from it. That is the whole of
/// M10's named risk, and `Generator::expand_after_sweep()` is what follows it.
/// Where one cell's water goes: a cell inside this region, a boundary side, or nowhere.
///
/// Split out of the routing loop because it is the only branchy part of it, and because "water
/// never leaves uphill" is a property of THIS function alone — the comparison against the
/// neighbour's own edge cell is what makes the cross-region flow graph acyclic, and therefore what
/// makes the sweep below converge to a unique fixed point.
struct FlowTarget {
    i32 cell = -1;  ///< A cell of this region, or -1.
    i32 side = -1;  ///< A boundary side, or -1. Both -1 means the cell is a sink.
};

[[nodiscard]] FlowTarget lowest_neighbour(const EvalArgs& args, Span<const f32> height,
                                          AttributeId source, i32 cx, i32 cz) noexcept {
    FlowTarget target;
    f32 best = height[static_cast<usize>(cz) * kRegionCells + static_cast<usize>(cx)];
    for (u32 side = 0; side < kRegionSides; ++side) {
        const i32 tx = cx + kSideDx[side];
        const i32 tz = cz + kSideDz[side];
        const bool inside = tx >= 0 && tz >= 0 && tx < static_cast<i32>(kRegionCells) &&
                            tz < static_cast<i32>(kRegionCells);
        if (inside) {
            const f32 candidate =
                height[static_cast<usize>(tz) * kRegionCells + static_cast<usize>(tx)];
            if (candidate < best) {
                best = candidate;
                target = FlowTarget{tz * static_cast<i32>(kRegionCells) + tx, -1};
            }
            continue;
        }
        // Outside the region: compare against the NEIGHBOUR'S OWN EDGE CELL so water does not leave
        // uphill. A missing neighbour is the world edge, and water leaves the world.
        const RegionState* other = args.world.find(RegionCoord{
            args.region.x + kSideDx[side], args.region.z + kSideDz[side], args.region.level});
        if (other == nullptr) {
            continue;
        }
        const u32 ex = clamp_cell(tx < 0 ? static_cast<i32>(kRegionCells) - 1
                                         : (tx >= static_cast<i32>(kRegionCells) ? 0 : tx));
        const u32 ez = clamp_cell(tz < 0 ? static_cast<i32>(kRegionCells) - 1
                                         : (tz >= static_cast<i32>(kRegionCells) ? 0 : tz));
        const f32 candidate = other->raster.at(source, ex, ez);
        if (candidate < best) {
            best = candidate;
            target = FlowTarget{-1, static_cast<i32>(side)};
        }
    }
    return target;
}

/// Descending height, ties broken by the cell index.
///
/// A TOTAL order, so the result does not depend on whether the sort is stable — which is what stops
/// a library change from moving a river. Descending height is also a topological order for
/// strictly-downhill flow, which is why one pass over it settles the region's interior.
void order_by_height(Span<const f32> height, u32* order) noexcept {
    for (u32 index = 0; index < kRegionCellCount; ++index) {
        order[index] = index;
    }
    std::sort(order, order + kRegionCellCount, [&height](u32 a, u32 b) {
        return height[a] != height[b] ? height[a] > height[b] : a < b;
    });
}

[[nodiscard]] Status record_neighbour_reads(const EvalArgs& args) noexcept {
    for (u32 side = 0; side < kRegionSides; ++side) {
        if (Status recorded = args.reads.read(RegionCoord{
                args.region.x + kSideDx[side], args.region.z + kSideDz[side], args.region.level});
            !recorded) {
            return recorded;
        }
    }
    return ok();
}

[[nodiscard]] Status eval_propagate(const EvalArgs& args, RegionState& state) noexcept {
    const AttributeId source = input_channel(args, 0);
    const Span<const f32> height = state.raster.values(source);
    const Span<f32> accumulation = state.raster.values_mutable(args.stage.output);
    if (height.size() != kRegionCellCount || accumulation.size() != kRegionCellCount) {
        return Status{make_unexpected(
            Error{ErrorCode::Internal, "pcg: a propagate stage needs a full region raster"})};
    }
    f32 water[kRegionCellCount] = {};
    gather_inflow(args, water);
    if (Status recorded = record_neighbour_reads(args); !recorded) {
        return recorded;
    }

    u32 order[kRegionCellCount];
    order_by_height(height, order);

    f32 leaving[kRegionSides * kRegionCells] = {};
    for (u32 index = 0; index < kRegionCellCount; ++index) {
        const u32 cell = order[index];
        const i32 cx = static_cast<i32>(cell % kRegionCells);
        const i32 cz = static_cast<i32>(cell / kRegionCells);
        const f32 total = 1.0F + water[cell];
        accumulation[cell] = total;

        const FlowTarget target = lowest_neighbour(args, height, source, cx, cz);
        if (target.cell >= 0) {
            water[static_cast<usize>(target.cell)] += total;
        } else if (target.side >= 0) {
            const u32 pos = (target.side == 1 || target.side == 3) ? static_cast<u32>(cz)
                                                                   : static_cast<u32>(cx);
            leaving[static_cast<u32>(target.side) * kRegionCells + pos] += total;
        }
    }
    for (usize index = 0; index < kRegionSides * kRegionCells; ++index) {
        state.outflow[index] = leaving[index];
    }
    return ok();
}

void eval_compute(const EvalArgs& args, RegionState& state) noexcept {
    const Span<const f32> input = state.raster.values(input_channel(args, 0));
    // TWO inputs is a BLEND — `a * value + b * second` — and one is an affine map of a single
    // channel. Both are the same instruction with a missing operand, which is why `Compute` admits
    // a range of arities rather than there being a second node kind for the blend.
    const Span<const f32> second = args.stage.input_count > 1
                                       ? state.raster.values(input_channel(args, 1))
                                       : Span<const f32>();
    const Span<f32> values = state.raster.values_mutable(args.stage.output);
    for (usize index = 0; index < values.size() && index < input.size(); ++index) {
        const f32 base = input[index];
        if (args.stage.params.invert) {
            values[index] = base != 0.0F ? 1.0F / base : 0.0F;
            continue;
        }
        const f32 addend = index < second.size() ? second[index] * args.stage.params.second
                                                 : args.stage.params.second;
        values[index] = base * args.stage.params.value + addend;
    }
}

}  // namespace

// --- The point stages -------------------------------------------------------------------------

namespace {

/// The raster value under a region-local position, in metres.
[[nodiscard]] f32 raster_at_local(const RegionState& state, AttributeId channel, f32 x, f32 z,
                                  f64 cell_metres) noexcept {
    const i32 cx = static_cast<i32>(std::floor(static_cast<f64>(x) / cell_metres));
    const i32 cz = static_cast<i32>(std::floor(static_cast<f64>(z) / cell_metres));
    return state.raster.at(channel, clamp_cell(cx), clamp_cell(cz));
}

/// Candidate generation, and the ONLY node that mints identity.
///
/// The slot is the candidate's index in this node's own sequence, taken BEFORE the density test —
/// so a candidate that survives keeps the same identity whether or not the ones before it did,
/// which is the difference between `derived` (0 of 7 877 overrides mis-bound) and a rank among
/// survivors (292).
[[nodiscard]] Status eval_scatter(const EvalArgs& args, RegionState& state,
                                  ProvenanceMode provenance) noexcept {
    const AttributeId density_channel = input_channel(args, 0);
    const RegionKey key = region_key(args.region.x, args.region.z, args.region.level);
    const RandomStream stream = generation_stream(args.seed, args.stage.identity, key);
    const f64 span = args.cell_metres * static_cast<f64>(kRegionCells);
    // The scratch was sized for this in `Generator::create()`; the resize is free and is here so
    // that a stage which ran before this one and shrank the buffers cannot shorten the batch.
    if (Status sized = args.query_x.resize(args.stage.params.count); !sized) {
        return sized;
    }
    if (Status sized = args.query_z.resize(args.stage.params.count); !sized) {
        return sized;
    }
    if (Status sized = args.query_f.resize(args.stage.params.count); !sized) {
        return sized;
    }
    state.candidates.clear();
    u32 produced = 0;
    for (u64 slot = 0; slot < args.stage.params.count; ++slot) {
        const f32 x =
            stream.unit_float(generation_point(), 0, slot * 4 + 0) * static_cast<f32>(span);
        const f32 z =
            stream.unit_float(generation_point(), 0, slot * 4 + 1) * static_cast<f32>(span);
        const f32 roll = stream.unit_float(generation_point(), 0, slot * 4 + 2);
        const f32 density = raster_at_local(state, density_channel, x, z, args.cell_metres);
        if (roll >= density) {
            if (provenance == ProvenanceMode::On) {
                RejectionRecord record;
                record.region = args.region;
                record.slot = static_cast<u32>(slot);
                record.stage = args.stage_index;
                record.stage_name = args.stage.name;
                record.tested_attribute = density_channel;
                record.tested_value = density;
                record.threshold = roll;
                record.margin = density - roll;
                record.position_x = x;
                record.position_z = z;
                if (Status recorded = state.provenance.record_rejected(record); !recorded) {
                    return recorded;
                }
            }
            continue;
        }
        Expected<u32, Error> index = state.candidates.add(x, 0.0F, z, static_cast<u32>(slot));
        if (!index) {
            return Status{make_unexpected(index.error())};
        }
        state.candidates.set_identity(
            *index,
            derive_identity(args.seed, args.stage.identity, key, static_cast<u32>(slot)).value);
        state.candidates.set_f32(args.program.density_attribute(), *index, density);
        state.candidates.set_u64(args.program.priority_attribute(), *index,
                                 stream.draw(generation_point(), 1, slot));
        args.query_x[produced] = static_cast<f64>(x);
        args.query_z[produced] = static_cast<f64>(z);
        ++produced;
    }
    // ONE batched query for the whole region, never one per point: "Queries SHALL be batchable,
    // since generation issues them in bulk."
    const SpatialQuery* geometry = args.context.geometry;
    if (geometry != nullptr && produced != 0) {
        for (u32 index = 0; index < produced; ++index) {
            args.query_x[index] += args.origin_x;
            args.query_z[index] += args.origin_z;
        }
        geometry->height_batch(Span<const f64>(args.query_x.data(), produced),
                               Span<const f64>(args.query_z.data(), produced),
                               Span<f32>(args.query_f.data(), produced));
        for (u32 index = 0; index < produced; ++index) {
            state.candidates.set_y(index, args.query_f[index]);
        }
    }
    return ok();
}

/// A threshold test, plus every filter the compiler fused into it.
/// One threshold test: at or above `lower`, and at or below `upper` when the test is a range.
[[nodiscard]] bool passes(f32 value, f32 lower, f32 upper, bool range_test) noexcept {
    return range_test ? (value >= lower && value <= upper) : (value >= lower);
}

/// Keep the rejection, with the value tested and the threshold it failed.
///
/// `procedural-content-generation` — "why is nothing here" — wants the filter NAMED and the
/// shortfall MEASURED, and a rejected candidate leaves no trace in the output at all, so this is
/// the only place the answer can come from.
[[nodiscard]] Status note_rejection(const EvalArgs& args, RegionState& state, usize index,
                                    AttributeId tested, f32 value, f32 threshold) noexcept {
    RejectionRecord record;
    record.region = args.region;
    record.slot = state.candidates.slot(index);
    record.stage = args.stage_index;
    record.stage_name = args.stage.name;
    record.tested_attribute = tested;
    record.tested_value = value;
    record.threshold = threshold;
    record.margin = value - threshold;
    record.position_x = state.candidates.x(index);
    record.position_z = state.candidates.z(index);
    return state.provenance.record_rejected(record);
}

/// Every test this stage carries, applied to one candidate: the filters the compiler FUSED into it
/// innermost outward, then the node's own. That is the order the author wrote, and therefore the
/// order a rejection should be attributed in.
[[nodiscard]] Expected<bool, Error> survives_filters(const EvalArgs& args, RegionState& state,
                                                     usize index,
                                                     ProvenanceMode provenance) noexcept {
    for (u8 fused = args.stage.fused_count; fused > 0; --fused) {
        const Stage::FusedFilter& test = args.stage.fused[fused - 1];
        const f32 value = state.candidates.get_f32(test.reads, index);
        if (passes(value, test.lower, test.upper, test.range_test)) {
            continue;
        }
        if (provenance == ProvenanceMode::On) {
            if (Status kept = note_rejection(args, state, index, test.reads, value, test.lower);
                !kept) {
                return make_unexpected(kept.error());
            }
        }
        return false;
    }
    const f32 value = state.candidates.get_f32(args.stage.reads, index);
    if (passes(value, args.stage.params.value, args.stage.params.second,
               args.stage.params.range_test)) {
        return true;
    }
    if (provenance == ProvenanceMode::On) {
        if (Status kept = note_rejection(args, state, index, args.stage.reads, value,
                                         args.stage.params.value);
            !kept) {
            return make_unexpected(kept.error());
        }
    }
    return false;
}

[[nodiscard]] Status eval_filter(const EvalArgs& args, RegionState& state,
                                 ProvenanceMode provenance) noexcept {
    PointSet& points = state.candidates;
    if (Status sized = args.keep.resize(points.size()); !sized) {
        return sized;
    }
    for (usize index = 0; index < points.size(); ++index) {
        Expected<bool, Error> survives = survives_filters(args, state, index, provenance);
        if (!survives) {
            return Status{make_unexpected(survives.error())};
        }
        args.keep[index] = *survives ? 1 : 0;
    }
    // IN PLACE. A filter that built a second point set would allocate one per region per
    // regeneration, and the survivors keep their ORIGINAL slots — which is what stops a rank among
    // survivors from being invented here by accident.
    return points.retain(Span<const u8>(args.keep.data(), points.size()));
}

/// What one region's candidates are contested against, gathered once per stage rather than
/// re-derived per candidate.
struct SpacingRules {
    f32 spacing = 0.0F;
    f64 spacing_squared = 0.0;
    AttributeId priority;
    AttributeId density;
    i32 reach = 0;
    f64 span = 0.0;
};

/// Who beat a candidate, if anyone did.
struct SpacingVerdict {
    bool rejected = false;
    GeneratedId beaten_by;
    RegionCoord beaten_in;
};

/// Does any STRICTLY HIGHER PRIORITY candidate within the spacing stand in the neighbourhood?
///
/// It reads CANDIDATE lists and never accepted points — the spike's first condition, and the
/// difference between reproducing a full regeneration in 12 of 12 trials and in 2 of 12: in a full
/// run a region later in traversal has nothing accepted yet, and in a partial run the cache is
/// already holding its points. `program.h` refuses a node that declares it wants the accepted set;
/// this function could not use one if it had it.
[[nodiscard]] SpacingVerdict contest(const EvalArgs& args, const RegionState& state,
                                     const SpacingRules& rules, usize index) noexcept {
    const f64 wx = args.origin_x + static_cast<f64>(state.candidates.x(index));
    const f64 wz = args.origin_z + static_cast<f64>(state.candidates.z(index));
    const u64 mine = state.candidates.get_u64(rules.priority, index);
    for (i32 dz = -rules.reach; dz <= rules.reach; ++dz) {
        for (i32 dx = -rules.reach; dx <= rules.reach; ++dx) {
            const RegionCoord neighbour{args.region.x + dx, args.region.z + dz, args.region.level};
            const RegionState* other = args.world.find(neighbour);
            if (other == nullptr) {
                continue;
            }
            const f64 other_x = args.origin_x + static_cast<f64>(dx) * rules.span;
            const f64 other_z = args.origin_z + static_cast<f64>(dz) * rules.span;
            for (usize slot = 0; slot < other->candidates.size(); ++slot) {
                if (other->candidates.get_u64(rules.priority, slot) <= mine) {
                    continue;  // ties and lower priorities never win; priority is a 64-bit draw
                }
                const f64 ddx = other_x + static_cast<f64>(other->candidates.x(slot)) - wx;
                const f64 ddz = other_z + static_cast<f64>(other->candidates.z(slot)) - wz;
                if (ddx * ddx + ddz * ddz < rules.spacing_squared) {
                    return SpacingVerdict{true, GeneratedId{other->candidates.identity(slot)},
                                          neighbour};
                }
            }
        }
    }
    return SpacingVerdict{};
}

/// A spacing rejection tests a DISTANCE rather than an attribute, so the record names the winner
/// and the region it came from instead of a value and a column — which is also what lets the suite
/// count CROSS-REGION contention rather than assume it.
[[nodiscard]] Status note_spacing_rejection(const EvalArgs& args, RegionState& state, usize index,
                                            const SpacingRules& rules,
                                            const SpacingVerdict& verdict) noexcept {
    RejectionRecord record;
    record.region = args.region;
    record.slot = state.candidates.slot(index);
    record.stage = args.stage_index;
    record.stage_name = args.stage.name;
    record.threshold = rules.spacing;
    record.position_x = state.candidates.x(index);
    record.position_z = state.candidates.z(index);
    record.beaten_by = verdict.beaten_by;
    record.beaten_in_region = verdict.beaten_in;
    return state.provenance.record_rejected(record);
}

/// Move one surviving candidate into the accepted set, identity and slot intact.
[[nodiscard]] Status accept_one(RegionState& state, const SpacingRules& rules,
                                usize index) noexcept {
    Expected<u32, Error> at =
        state.accepted.add(state.candidates.x(index), state.candidates.y(index),
                           state.candidates.z(index), state.candidates.slot(index));
    if (!at) {
        return Status{make_unexpected(at.error())};
    }
    state.accepted.set_identity(*at, state.candidates.identity(index));
    state.accepted.set_f32(rules.density, *at, state.candidates.get_f32(rules.density, index));
    state.accepted.set_u64(rules.priority, *at, state.candidates.get_u64(rules.priority, index));
    return ok();
}

[[nodiscard]] Status record_neighbourhood_reads(const EvalArgs& args, i32 reach) noexcept {
    for (i32 dz = -reach; dz <= reach; ++dz) {
        for (i32 dx = -reach; dx <= reach; ++dx) {
            if (Status recorded = args.reads.read(
                    RegionCoord{args.region.x + dx, args.region.z + dz, args.region.level});
                !recorded) {
                return recorded;
            }
        }
    }
    return ok();
}

/// ORDER-FREE conflict resolution. See `contest()` for why it reads candidates and not output.
[[nodiscard]] Status eval_spacing(const EvalArgs& args, RegionState& state,
                                  ProvenanceMode provenance) noexcept {
    SpacingRules rules;
    rules.spacing = args.stage.params.frequency;
    rules.spacing_squared = static_cast<f64>(rules.spacing) * static_cast<f64>(rules.spacing);
    rules.priority = args.program.priority_attribute();
    rules.density = args.program.density_attribute();
    rules.reach = static_cast<i32>(args.stage.reach_regions);
    rules.span = args.cell_metres * static_cast<f64>(kRegionCells);

    state.accepted.clear();
    for (usize index = 0; index < state.candidates.size(); ++index) {
        const SpacingVerdict verdict = contest(args, state, rules, index);
        if (!verdict.rejected) {
            if (Status accepted = accept_one(state, rules, index); !accepted) {
                return accepted;
            }
            continue;
        }
        if (provenance == ProvenanceMode::On) {
            if (Status kept = note_spacing_rejection(args, state, index, rules, verdict); !kept) {
                return kept;
            }
        }
    }
    return record_neighbourhood_reads(args, rules.reach);
}

}  // namespace

// --- The stage driver -------------------------------------------------------------------------

namespace {

/// Copy the candidate set into the accepted set. What a program with no `Spacing` stage does at its
/// output: the candidates ARE the result, and a consumer must not have to know which shape of
/// program produced the points it is holding.
[[nodiscard]] Status accept_candidates(RegionState& state, AttributeId density,
                                       AttributeId priority) noexcept {
    state.accepted.clear();
    for (usize index = 0; index < state.candidates.size(); ++index) {
        Expected<u32, Error> at =
            state.accepted.add(state.candidates.x(index), state.candidates.y(index),
                               state.candidates.z(index), state.candidates.slot(index));
        if (!at) {
            return Status{make_unexpected(at.error())};
        }
        state.accepted.set_identity(*at, state.candidates.identity(index));
        state.accepted.set_f32(density, *at, state.candidates.get_f32(density, index));
        state.accepted.set_u64(priority, *at, state.candidates.get_u64(priority, index));
    }
    return ok();
}

[[nodiscard]] Status record_accepted_provenance(const EvalArgs& args, RegionState& state) noexcept {
    for (usize index = 0; index < state.accepted.size(); ++index) {
        ProvenanceRecord record;
        record.identity = GeneratedId{state.accepted.identity(index)};
        record.region = args.region;
        record.seed = args.seed;
        record.generator_version = args.program.version();
        record.stage = args.stage_index;
        record.stage_name = args.stage.name;
        record.generator_name = args.program.name();
        record.slot = state.accepted.slot(index);
        record.deciding_attribute = args.program.density_attribute();
        record.deciding_value = state.accepted.get_f32(args.program.density_attribute(), index);
        record.position_x = state.accepted.x(index);
        record.position_z = state.accepted.z(index);
        if (Status recorded = state.provenance.record_accepted(record); !recorded) {
            return recorded;
        }
    }
    return ok();
}

/// The `Output` stage: settle the accepted set, take the macro summary, record provenance and hand
/// the result to the adapter.
[[nodiscard]] Status finalise_region(const EvalArgs& args, RegionState& state) noexcept {
    if (args.program.spacing_stage() == Program::kNoStage) {
        if (Status accepted = accept_candidates(state, args.program.density_attribute(),
                                                args.program.priority_attribute());
            !accepted) {
            return accepted;
        }
    }
    // The macro summary, taken from the SAME raster the detail came from — which is what makes
    // "materialisation is consistent with macro state" true by construction.
    state.macro_density = channel_mean(state.raster, args.program.macro_density_channel());
    state.macro_height = channel_mean(state.raster, args.program.macro_height_channel());
    state.macro_instances = static_cast<u32>(state.accepted.size());

    if (args.context.provenance == ProvenanceMode::On) {
        if (Status recorded = record_accepted_provenance(args, state); !recorded) {
            return recorded;
        }
    }
    if (args.context.outputs == nullptr) {
        return ok();
    }
    EmitContext emit;
    emit.region = state.coord;
    emit.origin_x = args.origin_x;
    emit.origin_z = args.origin_z;
    emit.region_metres = args.program.region_metres();
    emit.seed = args.context.seed;
    emit.generator_version = args.program.version();
    emit.generator_name = args.program.name();
    emit.attributes = &args.program.attributes();
    return args.context.outputs->emit(args.context.output_target, emit, state.accepted);
}

}  // namespace

Expected<bool, Error> Generator::evaluate_one(u8 stage_index, const RegionCoord& region) noexcept {
    Expected<RegionState*, Error> slot = world_->ensure(region);
    if (!slot) {
        return make_unexpected(slot.error());
    }
    RegionState& state = **slot;
    if (state.stage_digests.size() != program_.stages().size()) {
        if (Status sized = state.stage_digests.resize(program_.stages().size()); !sized) {
            return make_unexpected(sized.error());
        }
    }
    if (state.raster.edge() != kRegionCells) {
        if (Status sized = state.raster.reset(kRegionCells, program_.raster_channels()); !sized) {
            return make_unexpected(sized.error());
        }
    }

    const Stage& stage = program_.stages()[stage_index];
    const f64 cell_metres = program_.region_metres() / static_cast<f64>(kRegionCells);
    f64 origin_x = 0.0;
    f64 origin_z = 0.0;
    region_origin(context_, region, origin_x, origin_z);

    if (Status opened = reads_.begin(stage_index, region); !opened) {
        return make_unexpected(opened.error());
    }
    EvalArgs args{program_, context_,    stage,         stage_index,  region, origin_x,
                  origin_z, cell_metres, context_.seed, *world_,      reads_, query_x_,
                  query_z_, query_f_,    query_g_,      query_flags_, keep_};

    // The candidate set's columns are declared once per region: the priority and density columns
    // the evaluator owns, sized for the scatter's declared count. `PointSet::add()` refuses past
    // that, which is what makes "no per-point heap allocation" a checkable property.
    if (stage.kind == NodeKind::Scatter && state.candidates.capacity() != stage.params.count) {
        const AttributeDecl columns[2] = {
            AttributeDecl{"pcg.density", program_.density_attribute(), AttributeType::F32},
            AttributeDecl{"pcg.priority", program_.priority_attribute(), AttributeType::U64}};
        if (Status reserved =
                state.candidates.reserve(stage.params.count, Span<const AttributeDecl>(columns, 2));
            !reserved) {
            return make_unexpected(reserved.error());
        }
        if (Status reserved =
                state.accepted.reserve(stage.params.count, Span<const AttributeDecl>(columns, 2));
            !reserved) {
            return make_unexpected(reserved.error());
        }
    }
    if (state.touched_run != run_id_) {
        state.touched_run = run_id_;
        // Once per run, whichever stage reaches the region first. Keyed on "stage zero" it would
        // leave a region entered at a later stage accumulating a second copy of its rejections.
        state.provenance.clear();
    }

    const u64 before = state.stage_digests[stage_index];
    Status evaluated = ok();
    switch (stage.kind) {
        case NodeKind::Constant:
            eval_constant(args, state);
            break;
        case NodeKind::Noise:
            eval_noise(args, state);
            break;
        case NodeKind::FieldRead:
            evaluated = eval_field_read(args, state);
            break;
        case NodeKind::Stamp:
            eval_stamp(args, state);
            break;
        case NodeKind::Smooth:
            evaluated = eval_smooth(args, state);
            break;
        case NodeKind::Propagate:
            evaluated = eval_propagate(args, state);
            break;
        case NodeKind::Compute:
            eval_compute(args, state);
            break;
        case NodeKind::Scatter:
            evaluated = eval_scatter(args, state, context_.provenance);
            break;
        case NodeKind::Filter:
            evaluated = eval_filter(args, state, context_.provenance);
            break;
        case NodeKind::Spacing:
            evaluated = eval_spacing(args, state, context_.provenance);
            break;
        case NodeKind::Script:
            // A scripted node is a barrier the compiler reports and an identity transformation
            // here. It exists so its COST is visible; giving it semantics of its own would be a
            // general unbounded loop in the graph, which "Bounded iteration" forbids in as many
            // words.
            break;
        case NodeKind::Output:
            evaluated = finalise_region(args, state);
            break;
        case NodeKind::kCount:
            break;
    }
    reads_.end();
    if (!evaluated) {
        return make_unexpected(evaluated.error());
    }

    u64 after = 0;
    if (produces_raster(stage.kind)) {
        after = state.raster.digest_of(stage.output);
        if (stage.kind == NodeKind::Propagate) {
            Digest digest;
            digest.u64_value(after);
            digest.bytes(state.outflow, sizeof(state.outflow));
            after = digest.value();
        }
    } else if (stage.kind == NodeKind::Spacing || stage.kind == NodeKind::Output) {
        after = state.accepted.digest();
    } else {
        after = state.candidates.digest();
    }
    state.stage_digests[stage_index] = after;
    state.evaluated_mask |= 1ULL << stage_index;

    StageProfile* profile = profile_.stage_at(stage_index);
    if (profile != nullptr) {
        profile->regions_evaluated += 1;
        profile->candidates_in += state.candidates.size();
        profile->candidates_out += state.accepted.size();
        profile->bytes = state.bytes();
    }
    return after != before;
}

// --- Dirty sets, the fixed point, and the budgeted step ---------------------------------------

Status Generator::seed_dirty(const RegionSet& seeds, DirtyCause cause, u8 stage) noexcept {
    if (stage >= dirty_.size()) {
        return Status{
            make_unexpected(Error{ErrorCode::OutOfRange,
                                  "pcg: a dirty set was seeded at a stage the program does "
                                  "not have"})};
    }
    // The stage's own reach, applied to the seed set: an edit at the edge of a region is read by
    // the neighbour that gathers across the boundary.
    Expected<RegionSet, Error> dilated =
        dilate(*allocator_, seeds, program_.stages()[stage].reach_regions);
    if (!dilated) {
        return Status{make_unexpected(dilated.error())};
    }
    Expected<usize, Error> added = dirty_[stage].unite(*dilated);
    if (!added) {
        return Status{make_unexpected(added.error())};
    }

    if (ledger_.enabled()) {
        Array<RegionCoord> members(*allocator_);
        if (Status listed = dilated->members(members); !listed) {
            return listed;
        }
        for (const RegionCoord& region : members) {
            DirtyReason reason;
            reason.region = region;
            reason.stage = stage;
            reason.stage_name = program_.stages()[stage].name;
            reason.cause = seeds.contains(region) ? cause : DirtyCause::DeclaredReach;
            reason.source = region;
            if (Status recorded = ledger_.record(reason); !recorded) {
                return recorded;
            }
        }
    }
    profile_.regions_in_dirty_set += dirty_[stage].size();
    return ok();
}

Status Generator::start_at(u8 stage) noexcept {
    stage_ = stage;
    cursor_ = 0;
    cursor_members_.clear();
    if (stage_ < program_.stages().size()) {
        if (Status listed = dirty_[stage_].members(cursor_members_); !listed) {
            return listed;
        }
    }
    running_ = true;
    return ok();
}

u8 Generator::stage_of(NodeIdentity node) const noexcept {
    for (usize index = 0; index < program_.stages().size(); ++index) {
        if (program_.stages()[index].identity == node) {
            return static_cast<u8>(index);
        }
    }
    return Program::kNoStage;
}

Status Generator::prepare(ExecutionDomain domain, const GenerationContext& context) noexcept {
    if (!program_.may_run_in(domain)) {
        // "A generator not declared for a domain SHALL NOT run in it. A city generator intended for
        // cooking SHALL NOT be invocable at runtime by accident."
        return Status{make_unexpected(Error{ErrorCode::PermissionDenied,
                                            "pcg: this generator is not declared for the execution "
                                            "domain it was asked to run in"})};
    }
    context_ = context;
    domain_ = domain;
    if (context_.geometry == nullptr) {
        static const FlatSpatialQuery kFlat;
        context_.geometry = &kFlat;
    }
    ledger_.clear();
    profile_.reset();
    ++run_id_;
    cancelled_ = false;
    iteration_exhausted_ = false;
    sweep_ = 0;
    sweep_changed_ = false;
    for (RegionSet& set : dirty_) {
        set.clear();
    }
    for (RegionSet& set : changed_) {
        set.clear();
    }
    for (RegionSet& set : sweep_changed_set_) {
        set.clear();
    }
    return ok();
}

Status Generator::refuse_incomplete_world() const noexcept {
    if (program_.stages().empty() ||
        world_->complete_through(static_cast<u8>(program_.stages().size() - 1))) {
        return ok();
    }
    // A partial regeneration over an incomplete world would have a gather read a region with no
    // values, and would produce a result no full regeneration could reproduce. Refused by name
    // rather than answered plausibly.
    return Status{make_unexpected(
        Error{ErrorCode::Unavailable,
              "pcg: a partial regeneration needs a world that has been fully generated once; "
              "call generate_all() first"})};
}

Status Generator::record_change(u8 stage, const RegionCoord& region) noexcept {
    if (Status added = changed_[stage].add(region); !added) {
        return added;
    }
    if (Status added = sweep_changed_set_[stage].add(region); !added) {
        return added;
    }
    profile_.regions_actually_changed += 1;
    return ok();
}

Status Generator::expand_after_sweep(u8 stage) noexcept {
    // THE FIXED POINT. A region whose output changed in this sweep re-dirties everything within the
    // stage's declared reach of it, and the sweep runs again. The spike measured what happens
    // without this: a statically-closed invalidation reproduced a full regeneration in 7 of 12
    // trials — often enough that a casual test calls it sound, which is why this is the only
    // closure the module offers.
    Expected<RegionSet, Error> grown =
        dilate(*allocator_, sweep_changed_set_[stage], program_.stages()[stage].reach_regions);
    if (!grown) {
        return Status{make_unexpected(grown.error())};
    }
    Array<RegionCoord> added(*allocator_);
    if (Status listed = grown->members(added); !listed) {
        return listed;
    }
    u32 fresh = 0;
    for (const RegionCoord& region : added) {
        if (dirty_[stage].contains(region)) {
            continue;
        }
        if (Status inserted = dirty_[stage].add(region); !inserted) {
            return inserted;
        }
        ++fresh;
        DirtyReason reason;
        reason.region = region;
        reason.stage = stage;
        reason.stage_name = program_.stages()[stage].name;
        reason.cause = DirtyCause::FixedPointExpansion;
        reason.source = region;
        reason.hop = static_cast<u16>(sweep_ + 1);
        if (Status recorded = ledger_.record(reason); !recorded) {
            return recorded;
        }
    }
    StageProfile* profile = profile_.stage_at(stage);
    if (profile != nullptr) {
        profile->fixed_point_additions += fresh;
    }
    sweep_changed_set_[stage].clear();
    return ok();
}

Status Generator::carry_to_next_stage(u8 stage) noexcept {
    const u8 next = static_cast<u8>(stage + 1);
    if (next >= program_.stages().size()) {
        return ok();
    }
    const Stage& consumer = program_.stages()[next];

    // WHICH SET PROPAGATES, AND WHY IT IS NOT ALWAYS THE CHANGED ONE.
    //
    // A raster stage writes its OWN channel, so "this stage's output did not change" really does
    // mean the next stage's input is what it was, and the changed set is the right seed.
    //
    // A POINT stage does not. `Scatter`, `Filter` and `Spacing` all read and write the region's ONE
    // candidate buffer in place — a filter compacts it — so a scatter that re-ran and produced the
    // SAME candidates has still replaced a FILTERED buffer with an UNFILTERED one. Gating the next
    // stage on the digest there leaves the region holding candidates nothing trimmed, and a partial
    // regeneration then disagrees with the full one by exactly the regions whose scatter re-ran and
    // whose filter did not. This cost three regions of sixty-four before it was found, and the
    // digests said the stage had not changed, which is precisely why it was hard to see: the rule
    // is that DIGEST GATING IS ONLY SOUND WHEN EACH STAGE OWNS ITS OUTPUT.
    //
    // So a point stage propagates from the set it EVALUATED — `dirty_[stage]`, which for an
    // iterative stage has grown to everything the fixed point touched — rather than from what it
    // changed.
    const bool owns_output = produces_raster(program_.stages()[stage].kind);
    const RegionSet& source = owns_output ? changed_[stage] : dirty_[stage];

    Expected<RegionSet, Error> dilated = dilate(*allocator_, source, consumer.reach_regions);
    if (!dilated) {
        return Status{make_unexpected(dilated.error())};
    }
    Array<RegionCoord> members(*allocator_);
    if (Status listed = dilated->members(members); !listed) {
        return listed;
    }
    for (const RegionCoord& region : members) {
        const bool itself = source.contains(region);
        DirtyCause cause = itself ? DirtyCause::UpstreamStage : DirtyCause::DeclaredReach;
        if (!itself && reads_.has_record(next, region)) {
            // design.md §1.3: the long-range gather invalidates 179 regions of 576 to find one that
            // changed, and the answer is a record of what each region ACTUALLY READ rather than a
            // wider or narrower declared radius. Where a record exists, it narrows the declared
            // reach; where none does — a region never yet evaluated at this stage — the declared
            // reach stands, which is what keeps a first generation correct.
            bool reads_changed = false;
            for (const RegionCoord& read : reads_.reads_of(next, region)) {
                reads_changed = reads_changed || source.contains(read);
            }
            if (!reads_changed) {
                dilated->remove(region);
                continue;
            }
            cause = DirtyCause::RecordedRead;
        }
        DirtyReason reason;
        reason.region = region;
        reason.stage = next;
        reason.stage_name = consumer.name;
        reason.cause = cause;
        reason.source = region;
        if (Status recorded = ledger_.record(reason); !recorded) {
            return recorded;
        }
    }
    Expected<usize, Error> added = dirty_[next].unite(*dilated);
    if (!added) {
        return Status{make_unexpected(added.error())};
    }
    profile_.regions_in_dirty_set += dirty_[next].size();
    return ok();
}

Status Generator::advance_stage() noexcept {
    if (Status carried = carry_to_next_stage(stage_); !carried) {
        return carried;
    }
    ++stage_;
    sweep_ = 0;
    sweep_changed_ = false;
    cursor_ = 0;
    cursor_members_.clear();
    if (stage_ < program_.stages().size()) {
        return dirty_[stage_].members(cursor_members_);
    }
    return ok();
}

Status Generator::begin(ExecutionDomain domain, const GenerationContext& context,
                        const RegionSet& seeds, DirtyCause cause, u8 first_stage) noexcept {
    if (Status prepared = prepare(domain, context); !prepared) {
        return prepared;
    }
    if (Status seeded = seed_dirty(seeds, cause, first_stage); !seeded) {
        return seeded;
    }
    return start_at(first_stage);
}

Status Generator::begin_edit(ExecutionDomain domain, const GenerationContext& context,
                             Span<const AuthoredStamp> changed) noexcept {
    if (Status prepared = prepare(domain, context); !prepared) {
        return prepared;
    }
    u8 first = Program::kNoStage;
    for (const AuthoredStamp& stamp : changed) {
        const u8 stage = stage_of(stamp.node);
        if (stage == Program::kNoStage) {
            // A stamp naming a node the program does not have is an edit that would silently do
            // nothing. Named rather than ignored: "nothing happened" is the hardest defect to find
            // in a procedural world.
            return Status{make_unexpected(
                Error{ErrorCode::NotFound,
                      "pcg: an authored stamp names a node this compiled program does not have"})};
        }
        Expected<RegionSet, Error> touched =
            regions_touched(context_, Span<const AuthoredStamp>(&stamp, 1));
        if (!touched) {
            return Status{make_unexpected(touched.error())};
        }
        if (Status seeded = seed_dirty(*touched, DirtyCause::AuthoredEdit, stage); !seeded) {
            return seeded;
        }
        first = stage < first ? stage : first;
    }
    if (first == Program::kNoStage) {
        // No edits: nothing to regenerate, and a run that evaluates nothing is a valid answer. The
        // cursor is parked past the last stage so that a `step()` on this run reports Complete
        // rather than walking a stage index left over from the previous one.
        stage_ = static_cast<u8>(program_.stages().size());
        cursor_ = 0;
        cursor_members_.clear();
        running_ = false;
        return ok();
    }
    return start_at(first);
}

Expected<RunProgress, Error> Generator::step(u32 max_regions) noexcept {
    if (!running_) {
        RunProgress progress;
        progress.outcome = RunOutcome::Complete;
        return progress;
    }
    return drive(max_regions);
}

Expected<RunProgress, Error> Generator::drive(u32 max_regions) noexcept {
    RunProgress progress;
    progress.stage = stage_;
    while (stage_ < program_.stages().size()) {
        if (cancelled_) {
            // The dirty set and the cursor are intact, so resuming is the same call. "A runtime
            // generator SHALL NOT block simulation or presentation" needs a run that can be put
            // down without losing where it was.
            progress.outcome = RunOutcome::Cancelled;
            progress.stage = stage_;
            return progress;
        }
        while (cursor_ < cursor_members_.size()) {
            if (progress.regions_evaluated >= max_regions) {
                progress.outcome = RunOutcome::BudgetExhausted;
                progress.stage = stage_;
                return progress;
            }
            const RegionCoord region = cursor_members_[cursor_];
            ++cursor_;
            Expected<bool, Error> changed = evaluate_one(stage_, region);
            if (!changed) {
                return make_unexpected(changed.error());
            }
            if (*changed) {
                if (Status recorded = record_change(stage_, region); !recorded) {
                    return make_unexpected(recorded.error());
                }
                sweep_changed_ = true;
            }
            ++progress.regions_evaluated;
        }
        Expected<bool, Error> more = finish_pass();
        if (!more) {
            return make_unexpected(more.error());
        }
        if (*more) {
            continue;
        }
        if (Status advanced = advance_stage(); !advanced) {
            return make_unexpected(advanced.error());
        }
        progress.stage = stage_;
    }
    running_ = false;
    progress.outcome = RunOutcome::Complete;
    progress.iteration_exhausted = iteration_exhausted_;
    if (iteration_exhausted_) {
        // A `Convergence` bound is a REFUSAL POINT, not a budget: reaching it means the solve did
        // not converge, and a result that is not the fixed point is not the result. Returned as an
        // error rather than as a flag a caller may ignore.
        return make_unexpected(Error{ErrorCode::Internal,
                                     "pcg: an iterative stage declared convergence and reached its "
                                     "declared bound without converging"});
    }
    return progress;
}

Expected<bool, Error> Generator::finish_pass() noexcept {
    const Stage& stage = program_.stages()[stage_];
    if (stage.iteration == IterationPolicy::None) {
        return false;
    }
    const u32 sweeps_done = sweep_ + 1;
    const usize before = dirty_[stage_].size();
    if (stage.iteration == IterationPolicy::Convergence) {
        if (Status expanded = expand_after_sweep(stage_); !expanded) {
            return make_unexpected(expanded.error());
        }
    }
    const bool grew = dirty_[stage_].size() != before;
    const bool keep_going = stage.iteration == IterationPolicy::Budget
                                ? sweeps_done < stage.iteration_bound
                                : (sweep_changed_ || grew);
    StageProfile* profile = profile_.stage_at(stage_);
    if (profile != nullptr) {
        profile->sweeps = sweeps_done;
    }
    if (!keep_going) {
        return false;
    }
    if (sweeps_done >= stage.iteration_bound) {
        iteration_exhausted_ = stage.iteration == IterationPolicy::Convergence;
        return false;
    }
    ++sweep_;
    sweep_changed_ = false;
    cursor_ = 0;
    if (Status listed = dirty_[stage_].members(cursor_members_); !listed) {
        return make_unexpected(listed.error());
    }
    return true;
}

Expected<RunProgress, Error> Generator::generate_all(ExecutionDomain domain,
                                                     const GenerationContext& context) noexcept {
    RegionSet everything(*allocator_, world_->extent());
    if (Status sized = everything.resize(); !sized) {
        return make_unexpected(sized.error());
    }
    const RegionExtent& extent = world_->extent();
    for (i32 z = extent.min_z; z <= extent.max_z; ++z) {
        for (i32 x = extent.min_x; x <= extent.max_x; ++x) {
            if (Status added = everything.add(RegionCoord{x, z, extent.level}); !added) {
                return make_unexpected(added.error());
            }
        }
    }
    if (Status began = begin(domain, context, everything, DirtyCause::ProgramChanged, 0); !began) {
        return make_unexpected(began.error());
    }
    return drive(0xFFFFFFFFU);
}

Expected<RunProgress, Error> Generator::regenerate(ExecutionDomain domain,
                                                   const GenerationContext& context,
                                                   const RegionSet& seeds, DirtyCause cause,
                                                   u8 first_stage) noexcept {
    if (Status complete = refuse_incomplete_world(); !complete) {
        return make_unexpected(complete.error());
    }
    if (Status began = begin(domain, context, seeds, cause, first_stage); !began) {
        return make_unexpected(began.error());
    }
    return drive(0xFFFFFFFFU);
}

Expected<RunProgress, Error> Generator::regenerate_edit(
    ExecutionDomain domain, const GenerationContext& context,
    Span<const AuthoredStamp> changed) noexcept {
    if (Status complete = refuse_incomplete_world(); !complete) {
        return make_unexpected(complete.error());
    }
    if (Status began = begin_edit(domain, context, changed); !began) {
        return make_unexpected(began.error());
    }
    return drive(0xFFFFFFFFU);
}

Expected<RegionSet, Error> Generator::regions_touched(
    const GenerationContext& context, Span<const AuthoredStamp> stamps) const noexcept {
    RegionSet touched(*allocator_, world_->extent());
    if (Status sized = touched.resize(); !sized) {
        return make_unexpected(sized.error());
    }
    for (const AuthoredStamp& stamp : stamps) {
        const RegionCoord low = region_of(context, stamp.x - stamp.radius, stamp.z - stamp.radius);
        const RegionCoord high = region_of(context, stamp.x + stamp.radius, stamp.z + stamp.radius);
        for (i32 z = low.z; z <= high.z; ++z) {
            for (i32 x = low.x; x <= high.x; ++x) {
                if (Status added = touched.add(RegionCoord{x, z, low.level}); !added) {
                    return make_unexpected(added.error());
                }
            }
        }
    }
    return touched;
}

}  // namespace cy::pcg
