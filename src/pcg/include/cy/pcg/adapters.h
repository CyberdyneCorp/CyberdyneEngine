#pragma once
// Output adapters: the representation a generated result becomes. M10 task 4.3.
//
// `procedural-content-generation` — "Output adapters": "Generators SHALL produce results through
// output adapters, and the adapter SHALL determine the representation ... **A procedural result
// SHALL NOT be an entity by default.** A generator producing ten million trees SHALL produce a
// foliage population, not ten million spawn operations", and "Projects and plugins SHALL be able to
// register adapters through the extension points in `project-and-plugins`."
//
// ================================================================================================
// "NOT AN ENTITY BY DEFAULT" IS A PROPERTY OF THIS REGISTRY, NOT A CONVENTION
// ================================================================================================
//
// `OutputRegistry` STARTS WITH NOTHING BOUND — not even `Fields`, which this module could supply
// because it already depends on the substrate. Emitting to any target nobody registered fails with
// a diagnostic that names it, and `OutputTarget::Entities` is the one the specification cares
// about: a project that genuinely wants an entity per generated instance has to write that adapter
// and register it, at which point the decision is visible in its own source rather than in a
// default nobody read. `tests/test_adapters.cpp` holds the refusal as a case.
//
// ================================================================================================
// WHERE THE OTHER ADAPTERS LIVE, AND WHY THEY ARE NOT HERE
// ================================================================================================
//
// `cy::pcg` depends on `cy::environment` and `cy::world` and on nothing else above the core. A
// foliage adapter needs `cy::foliage` and a terrain adapter needs `cy::terrain`, and linking either
// from here would make every consumer of a PCG graph — a cook, a unit test, a dedicated server —
// link a renderer-facing vegetation module it has no use for. So they are a SECOND TARGET,
// `cy::pcg-adapters`, declared beside this one: `src/terrain/CMakeLists.txt` splits
// `cy::terrain-cook` off for the same reason and `src/foliage/CMakeLists.txt` splits
// `cy::foliage-render`.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/pcg/dataset.h>
#include <cy/pcg/identity.h>
#include <cy/pcg/invalidation.h>

namespace cy::environment {
class FieldStore;
class ProducerToken;
struct FieldDeclaration;
}  // namespace cy::environment

namespace cy::pcg {

/// The representations a generated result can become. The specification's own table.
enum class OutputTarget : u8 {
    Foliage = 0,
    Terrain,
    Fields,
    Entities,
    Splines,
    Geometry,
    WorldMetadata,
    kCount,
};

[[nodiscard]] const char* output_target_name(OutputTarget target) noexcept;

/// What an adapter is handed.
struct EmitContext {
    RegionCoord region;
    /// Absolute world metres of the region's minimum corner, so an adapter can place a
    /// region-local point in world space without knowing the region grid.
    f64 origin_x = 0.0;
    f64 origin_z = 0.0;
    f64 region_metres = 0.0;
    u64 seed = 0;
    u32 generator_version = 0;
    const char* generator_name = "";
    /// The attribute table, for an adapter that reads a named column — a species selector, a scale.
    const AttributeTable* attributes = nullptr;
};

/// One representation a generator can produce into.
///
/// Not a `std::function` and not a callback pair: an adapter holds state — a cluster builder, a
/// field writer, a spline network — and its lifetime is the caller's, which is the shape an
/// extension point in `project-and-plugins` has everywhere else in this engine.
class OutputAdapter {
public:
    OutputAdapter() = default;
    OutputAdapter(const OutputAdapter&) = delete;
    OutputAdapter& operator=(const OutputAdapter&) = delete;
    virtual ~OutputAdapter() = default;

    /// The adapter's own name, for a diagnostic. Never null.
    [[nodiscard]] virtual const char* name() const noexcept = 0;
    [[nodiscard]] virtual OutputTarget target() const noexcept = 0;

    /// Take one region's accepted points. Called once per region per regeneration, never once per
    /// point — which is the mechanical half of "ten million trees are a population".
    [[nodiscard]] virtual Status emit(const EmitContext& context,
                                      const PointSet& points) noexcept = 0;

    /// A region's detail is gone and its macro summary is what remains. The demotion half of
    /// "Macro state and materialisation"; an adapter that holds nothing per region ignores it.
    [[nodiscard]] virtual Status demote(const EmitContext&) noexcept { return ok(); }
};

/// Where a generator's output goes. Adapters are registered by the application, not found.
class OutputRegistry {
public:
    explicit OutputRegistry(Allocator& allocator) noexcept : slots_(allocator) {}

    OutputRegistry(const OutputRegistry&) = delete;
    OutputRegistry& operator=(const OutputRegistry&) = delete;

    /// Bind an adapter to its target. A second registration for one target is refused and names
    /// both adapters — the same refusal shape `environment::FieldRegistry::claim()` uses for a
    /// second producer, because two adapters writing one representation is the same defect one
    /// level up.
    [[nodiscard]] Status register_adapter(OutputAdapter& adapter) noexcept;

    [[nodiscard]] OutputAdapter* find(OutputTarget target) const noexcept;

    /// Emit to `target`. Fails, naming the target, when nothing is registered for it — which is how
    /// "a procedural result SHALL NOT be an entity by default" behaves rather than being asserted.
    [[nodiscard]] Status emit(OutputTarget target, const EmitContext& context,
                              const PointSet& points) const noexcept;

    [[nodiscard]] usize size() const noexcept;

private:
    Array<OutputAdapter*> slots_;
};

/// The one adapter this target can supply: a point set becomes field values.
///
/// `procedural-content-generation` — "Field integration": "Generators SHALL be able to WRITE fields
/// where they are the declared producer of one: a road generator writing road distance, a fire
/// simulation writing burn state, a terraforming system writing soil health."
///
/// THE PRODUCER TOKEN IS THE CALLER'S. This adapter takes a reference to one rather than claiming a
/// field itself, because `environment-fields` refuses a second producer at registration and a PCG
/// adapter that claimed on the caller's behalf would move that refusal to a place the caller cannot
/// see. A generator writing `road distance` is the road system's producer claim, held by the road
/// system.
class FieldOutputAdapter final : public OutputAdapter {
public:
    /// `radius_metres` is the distance over which each point's contribution falls to zero — a road
    /// generator's "distance to road" is a splat, not a point. `value` is what a point contributes
    /// at its centre.
    FieldOutputAdapter(environment::FieldStore& store, environment::ProducerToken& token,
                       f32 radius_metres, f32 value) noexcept;

    [[nodiscard]] const char* name() const noexcept override { return "pcg.field"; }
    [[nodiscard]] OutputTarget target() const noexcept override { return OutputTarget::Fields; }

    [[nodiscard]] Status emit(const EmitContext& context, const PointSet& points) noexcept override;

    /// Lattice points written by the last emit, for the suite and for the profiler.
    [[nodiscard]] u64 written() const noexcept { return written_; }

private:
    environment::FieldStore* store_;
    environment::ProducerToken* token_;
    f32 radius_ = 0.0F;
    f32 value_ = 1.0F;
    u64 written_ = 0;
};

/// An adapter that counts and keeps what it was given, for tests and for the region debugger.
///
/// Not a mock: the editor's region debugger needs exactly this — the points a region produced,
/// held, without a representation being built — and a suite that had to bring up a foliage cluster
/// store to check that a generator emitted would be testing the wrong module.
class RecordingAdapter final : public OutputAdapter {
public:
    RecordingAdapter(Allocator& allocator, OutputTarget target) noexcept
        : points_(allocator), identities_(allocator), target_(target) {}

    [[nodiscard]] const char* name() const noexcept override { return "pcg.recording"; }
    [[nodiscard]] OutputTarget target() const noexcept override { return target_; }

    [[nodiscard]] Status emit(const EmitContext& context, const PointSet& points) noexcept override;
    [[nodiscard]] Status demote(const EmitContext& context) noexcept override;

    [[nodiscard]] u64 emitted() const noexcept { return emitted_; }
    [[nodiscard]] u64 regions() const noexcept { return regions_; }
    [[nodiscard]] u64 demotions() const noexcept { return demotions_; }
    [[nodiscard]] Span<const GeneratedId> identities() const noexcept { return identities_.span(); }
    void reset() noexcept;

private:
    struct Emitted {
        RegionCoord region;
        u32 count = 0;
    };

    Array<Emitted> points_;
    Array<GeneratedId> identities_;
    OutputTarget target_ = OutputTarget::Foliage;
    u64 emitted_ = 0;
    u64 regions_ = 0;
    u64 demotions_ = 0;
};

}  // namespace cy::pcg
