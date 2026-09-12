#pragma once
// The VFX asset model, and the CyberGraph node library the stages are authored from. M8.c task 2.1.
//
// ================================================================================================
// WHAT AN ASSET IS, IN THE SPECIFICATION'S OWN WORDS
// ================================================================================================
//
// `vfx-system`: "A **VFX system** asset SHALL consist of: one or more **emitters**, a set of typed
// **parameters** exposed to gameplay and to the editor, declared **event channels**, an
// **importance class**, and a **scalability policy**." Every one of those five is a member of
// `VfxSystemAsset` below, and none of them is optional.
//
// An emitter declares six stages and each stage is a `cy::graph::Graph` — CyberGraph unchanged,
// which is the whole of what M8.b's spike said is shared. What is NOT shared is what a stage
// COMPILES TO: `ir.h` is VFX's own intermediate representation, and the reason is written there.
//
// ================================================================================================
// THE THREE DECLARATIONS THAT ARE COMPILER INPUTS RATHER THAN RUNTIME DATA
// ================================================================================================
//
// 1. A PARAMETER THAT IS NOT EXPOSED IS FOLDED. `vfx-system`: "WHEN an artist sets a module's
//    parameter to a constant and does not expose it THEN the value SHALL be folded into the
//    generated code rather than read from a buffer." So `ParameterDecl::exposed` is read by the
//    compiler, not by the runtime, and `CompileReport::folded_parameters` counts what it did.
//
// 2. AN ATTRIBUTE DECLARES A RANGE AND A TOLERANCE, AND THAT IS WHAT SELECTS ITS PRECISION.
//    "The compiler SHALL additionally select each attribute's storage precision from declared
//    ranges and usage WHERE A REDUCED PRECISION IS PROVABLY SUFFICIENT, with an explicit authoring
//    override." A tolerance is what makes "provably" a computation rather than a guess: the
//    compiler picks the smallest encoding whose representable step over the declared range is no
//    larger than the tolerance the author accepted. A tolerance of zero — the default — yields
//    `Float32` and nothing is quantised behind anybody's back. See `layout.h`.
//
// 3. AN EFFECT DECLARES WHICH SIMULATION PATH IT REQUIRES. "An effect SHALL declare which path it
//    requires. The CPU path SHALL NOT be presented as merely a degraded GPU path, because its use
//    cases differ." `SimulationPath::CpuRequired` is the effect whose per-particle results gameplay
//    reads synchronously; `GpuPreferred` is everything else and falls back only when the device
//    cannot, which `runtime.h` reports rather than hides.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/graph/cybergraph.h>

namespace cy::vfx {

using graph::Graph;
using graph::NodeRegistry;

/// The six stages `vfx-system`'s table declares. `Count` is the size of a per-stage array and never
/// a stage.
enum class Stage : u8 {
    /// Per spawn request: how many particles to create.
    Spawn = 0,
    /// Per newly created particle: initial attribute values.
    Initialise,
    /// Per live particle per simulation step.
    Update,
    /// Per received event.
    Event,
    /// Per particle at draw time: the renderer's inputs.
    Render,
    /// Per emitter per step: reductions, grids. The seam `vfx-system` reserves for fluids —
    /// "the scheduler's ability to host non-particle simulation stages within an effect".
    Compute,
    Count,
};

[[nodiscard]] const char* stage_name(Stage stage) noexcept;

/// `vfx-system`: "Every effect SHALL declare an importance class". The ORDER IS THE RANK and the
/// budget controller reads it as one: adjustment proceeds from the highest enumerator to the
/// lowest, which is `Decorative` first and `Critical` last.
enum class ImportanceClass : u8 { Critical = 0, Important, Ambient, Decorative, Count };

[[nodiscard]] const char* importance_name(ImportanceClass importance) noexcept;

inline constexpr u32 kImportanceCount = static_cast<u32>(ImportanceClass::Count);

/// Which of the CPU path's two distinct purposes an effect is using it for, or that it wants the
/// GPU. See the note at the top of this file.
enum class SimulationPath : u8 {
    /// GPU compute, falling back to the CPU only when the device cannot — reported, never silent.
    GpuPreferred = 0,
    /// The effect's per-particle results are read synchronously by gameplay. Its particle count is
    /// bounded by that choice and the documentation says so.
    CpuRequired,
};

[[nodiscard]] const char* path_name(SimulationPath path) noexcept;

/// How far the budget controller may reduce this effect, and what it may switch off.
///
/// A POLICY THE ASSET DECLARES rather than a quality preset the engine picks: `vfx-system` requires
/// cost to be "bounded by a frame-time budget with importance classes, rather than by a quality
/// preset", and a controller with no per-effect floor would reduce a `Critical` effect to nothing
/// while claiming to have held its budget.
struct ScalabilityPolicy {
    /// The smallest fraction of the authored spawn rate this effect may be reduced to.
    f32 min_spawn_scale = 0.25F;
    /// The lowest simulation frequency, in hertz. `vfx-system`'s "Distant smoke is cheap" scenario
    /// is an 8 Hz effect rendered at 120 FPS.
    f32 min_simulation_hz = 8.0F;
    /// Particles this effect keeps whatever the controller decides.
    u32 reserved_particles = 0;
    bool may_drop_sorting = true;
    bool may_reduce_collision = true;
    bool may_reduce_feature_level = true;
};

/// A parameter exposed to gameplay and to the editor. Four floats covers every type in `ir.h`'s
/// lattice, which is deliberate: a parameter block whose stride depends on the type is a parameter
/// block a shader cannot index.
struct ParameterDecl {
    Name name;
    /// A type name from `ir.h`'s lattice: `float`, `vec2`, `vec3`, `vec4`, `int`, `bool`.
    Name type;
    f32 value[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    /// False means the compiler folds `value` into the generated code. See note 1 above.
    bool exposed = true;
};

/// The storage encoding of one attribute. `Auto` asks the compiler; anything else is the explicit
/// authoring override `vfx-system` requires to exist.
enum class Precision : u8 { Auto = 0, Float32, Float16, Unorm8, Snorm16 };

[[nodiscard]] const char* precision_name(Precision precision) noexcept;
/// Bytes one component occupies at this precision. Zero for `Auto`, which is not an encoding.
[[nodiscard]] u32 precision_bytes(Precision precision) noexcept;

/// What an author says about an attribute so the compiler can choose its precision.
struct AttributeDecl {
    Name name;
    Name type;
    /// The range the author guarantees the attribute stays inside.
    f32 minimum = 0.0F;
    f32 maximum = 1.0F;
    /// The largest absolute error the author accepts in this attribute. ZERO — the default — means
    /// no error is acceptable and the compiler picks `Float32`. Nothing is quantised by default.
    f32 tolerance = 0.0F;
    Precision override_precision = Precision::Auto;
};

/// A declared event channel, with the two bounds `vfx-system` requires every channel to carry.
struct EventChannelDecl {
    Name name;
    /// "Every event channel SHALL declare a maximum events per frame".
    u32 max_events_per_frame = 1024;
    /// "and every chain a maximum depth".
    u32 max_chain_depth = 4;
    /// OPT-IN, and the documentation says it is not the default mechanism. A channel with this off
    /// is GPU-to-GPU and never reaches the CPU.
    bool readback = false;
};

/// One emitter: a name, six optional stage graphs, its attribute declarations, and the path it
/// requires.
class Emitter {
public:
    Emitter(Allocator& allocator, Name emitter_name) noexcept;

    Emitter(const Emitter&) = delete;
    Emitter& operator=(const Emitter&) = delete;
    Emitter(Emitter&&) noexcept = default;
    Emitter& operator=(Emitter&&) noexcept = default;

    [[nodiscard]] Name name() const noexcept { return name_; }

    /// Give a stage its graph. A stage already set is replaced, which is what an editor does.
    [[nodiscard]] Status set_stage(Stage which, Graph&& graph) noexcept;
    [[nodiscard]] const Graph* stage(Stage which) const noexcept;
    /// Resolve every stage graph's node types against `registry`. CyberGraph's load-time step: a
    /// node whose type the registry does not have keeps whatever body it was loaded with and is
    /// reported by `validate`, rather than being dropped. `compile_system` refuses an asset that
    /// has not been through this, because an unresolved graph validates as sixty-seven unknown
    /// nodes and that diagnostic blames the author for the caller's omission.
    void resolve(const NodeRegistry& registry) noexcept;
    [[nodiscard]] bool has_stage(Stage which) const noexcept { return stage(which) != nullptr; }

    [[nodiscard]] Status declare_attribute(const AttributeDecl& decl) noexcept;
    [[nodiscard]] Span<const AttributeDecl> attributes() const noexcept {
        return attributes_.span();
    }
    [[nodiscard]] const AttributeDecl* find_attribute(Name attribute) const noexcept;

    [[nodiscard]] SimulationPath path() const noexcept { return path_; }
    void set_path(SimulationPath path) noexcept { path_ = path; }

    /// WHICH RENDERER DRAWS THIS EMITTER, and the properties `vfx-system` requires every renderer
    /// to support. M10 task 5.3.
    ///
    /// An index into `vfx-system`'s renderer table rather than the `RendererKind` enumerator
    /// itself, because that enumerator lives in `renderers.h` with the publications — and this
    /// header is the asset model, which `cy::vfx-compiler` compiles without ever naming a
    /// publication. `renderer_kind_name` is the spelling; a value at or above
    /// `kRendererKindCount` is refused by the publication that reads it, not here, because the
    /// cook has no table to check it against either.
    [[nodiscard]] u8 renderer() const noexcept { return renderer_; }
    void set_renderer(u8 kind) noexcept { renderer_ = kind; }

    /// The particle count this emitter asks for at full quality.
    [[nodiscard]] u32 capacity() const noexcept { return capacity_; }
    void set_capacity(u32 capacity) noexcept { capacity_ = capacity; }

    [[nodiscard]] Allocator& allocator() const noexcept { return attributes_.allocator(); }

private:
    struct StageEntry {
        Stage stage = Stage::Count;
        Graph graph;
    };

    Name name_;
    Array<StageEntry> stages_;
    Array<AttributeDecl> attributes_;
    SimulationPath path_ = SimulationPath::GpuPreferred;
    /// `RendererKind::Sprite`. See `set_renderer`.
    u8 renderer_ = 0;
    u32 capacity_ = 1024;
};

/// The asset. Five members, and they are the five the specification lists.
class VfxSystemAsset {
public:
    VfxSystemAsset(Allocator& allocator, Name asset_name) noexcept;

    VfxSystemAsset(const VfxSystemAsset&) = delete;
    VfxSystemAsset& operator=(const VfxSystemAsset&) = delete;
    VfxSystemAsset(VfxSystemAsset&&) noexcept = default;
    VfxSystemAsset& operator=(VfxSystemAsset&&) noexcept = default;

    [[nodiscard]] Name name() const noexcept { return name_; }

    /// Resolve every emitter's every stage graph. See `Emitter::resolve`.
    void resolve(const NodeRegistry& registry) noexcept;

    [[nodiscard]] Status add_emitter(Emitter&& emitter) noexcept;
    [[nodiscard]] Span<const Emitter> emitters() const noexcept { return emitters_.span(); }
    [[nodiscard]] const Emitter* find_emitter(Name emitter) const noexcept;

    [[nodiscard]] Status declare_parameter(const ParameterDecl& decl) noexcept;
    [[nodiscard]] Span<const ParameterDecl> parameters() const noexcept {
        return parameters_.span();
    }
    [[nodiscard]] const ParameterDecl* find_parameter(Name parameter) const noexcept;

    [[nodiscard]] Status declare_channel(const EventChannelDecl& decl) noexcept;
    [[nodiscard]] Span<const EventChannelDecl> channels() const noexcept {
        return channels_.span();
    }
    [[nodiscard]] const EventChannelDecl* find_channel(Name channel) const noexcept;

    [[nodiscard]] ImportanceClass importance() const noexcept { return importance_; }
    void set_importance(ImportanceClass importance) noexcept { importance_ = importance; }

    [[nodiscard]] const ScalabilityPolicy& scalability() const noexcept { return scalability_; }
    void set_scalability(const ScalabilityPolicy& policy) noexcept { scalability_ = policy; }

    [[nodiscard]] Allocator& allocator() const noexcept { return emitters_.allocator(); }

private:
    Name name_;
    Array<Emitter> emitters_;
    Array<ParameterDecl> parameters_;
    Array<EventChannelDecl> channels_;
    ImportanceClass importance_ = ImportanceClass::Ambient;
    ScalabilityPolicy scalability_;
};

// --- The node library --------------------------------------------------------------------------
//
// The names an authored stage graph uses. Registered into a `NodeRegistry` at run time, which is
// CyberGraph's decision 1 — "a node type is DATA, not an enumerator" — and what lets a project add
// a module without this compiler changing.

/// Property names the lowering reads off a node. Spelled once, here, because a lowering that
/// interned `"attribute"` in three places would find the fourth spelling at run time.
namespace prop {
inline constexpr const char* kAttribute = "attribute";
inline constexpr const char* kParameter = "parameter";
inline constexpr const char* kValue = "value";
inline constexpr const char* kInterface = "interface";
inline constexpr const char* kField = "field";
inline constexpr const char* kChannel = "channel";
inline constexpr const char* kInput = "input";
inline constexpr const char* kCurve = "curve";
}  // namespace prop

/// Register the built-in VFX node types. Idempotent: registering twice is refused by the registry
/// itself, which is how a caller finds out it did.
[[nodiscard]] Status register_vfx_nodes(NodeRegistry& registry) noexcept;

}  // namespace cy::vfx
