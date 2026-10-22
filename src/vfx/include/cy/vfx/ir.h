#pragma once
// THE VFX INTERMEDIATE REPRESENTATION. VFX's own, and this file is where the reason is written.
// M8.c task 2.1.
//
// ================================================================================================
// WHY THIS IS NOT `cy::graph::Module` ALONE, AND NOT ANYBODY ELSE'S IR
// ================================================================================================
//
// M8.b's spike measured that one graph IR cannot serve seven consumers, and `design.md` §1 of this
// milestone repeats the finding for the two consumers M8.c owns. For VFX the blocker is the second
// of the five the spike listed:
//
//     "identity is content, so two reads of one mutable cell ARE one value — there is no Store"
//
// **A particle kernel writes attributes.** `position` at the end of an update step is not the
// `position` at the start, and a hash-consed pure-expression DAG cannot tell them apart, because
// telling them apart is exactly the property it gives up to get content identity. So a kernel is
// not an expression DAG.
//
// What a kernel IS, and it is the smallest structure that carries the difference:
//
//     an ORDERED list of attribute writes, each of whose VALUES is a pure expression.
//
// The ordered list is VFX's own and lives in this file. The values are pure, so they lower onto
// `cy::graph`'s shared expression core unchanged — which is `design.md` §1's row for the core
// exactly: "Where the sub-language is pure. A particle's per-attribute expression is pure and
// belongs on it. A timeline is not."
//
// Everything that follows from being on that core comes free and is required by `vfx-system`:
// **typed and SSA-formed** (`Builder::make` types every node and refuses one it cannot type),
// **constant folding**, **dead-code elimination** (a rebuild from the roots cannot reach an
// orphan), and **common-subexpression elimination** across the whole kernel. What the core does not
// do, and what `compile.cpp` therefore does, is **attribute liveness analysis** and **kernel
// fusion** — both are statements about the ordered write list, which is the half the core does not
// have.
//
// ================================================================================================
// THE WRITE LIST IS EXPRESSED AS DECLARED ROOTS, AND THAT IS NOT A TRICK
// ================================================================================================
//
// `RootDecl` is E3 of the core's four extensions: "A domain declares a typed list of roots". A
// kernel's roots are its writes. The list is FIXED-SIZE — `kMaxKernelWrites` — because the core's
// root table is the domain's and a domain is a static table; that bound is the same bound
// `layout.h` puts on an emitter's attribute count, and it is reported rather than silently
// truncated (`ErrorCode::OutOfRange`, naming the kernel).
//
// Two roots beyond the writes, because a kernel does two things that are not writes:
//
//   `kKillRoot`   the predicate that ends a particle's life. A root rather than a write, because
//                 killing is not an attribute and a `dead` attribute would be allocated per
//                 particle for a bit the scheduler already keeps.
//   `kSpawnRoot`  how many particles a Spawn stage asks for. The one root the Spawn stage sets and
//                 no other stage does.
//
// ================================================================================================
// WHAT THE RUNTIME EXECUTES, AND WHY IT IS NOT A GRAPH INTERPRETER
// ================================================================================================
//
// `vfx-system`: "the runtime SHALL contain no graph interpreter and no graph-to-source compiler".
// `KernelStep` below is the compiled form the CPU path steps through — a flat array of operations
// over slots, with no pointer to follow and no virtual call to make, which is the same shape
// `cy::graph::camera`'s `RigStep` has and for the same reason. It is produced BY the compiler from
// the IR; the runtime never sees a `cy::graph::Graph`, and the two targets in this directory's
// CMakeLists.txt make that structural rather than a promise.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/graph/expr.h>
#include <cy/vfx/asset.h>

namespace cy::vfx {

using graph::Domain;
using graph::Immediate;
using graph::kInvalidNode;
using graph::kInvalidOp;
using graph::kInvalidType;
using graph::Module;
using graph::NodeId;
using graph::OpId;
using graph::TypeId;

/// The VFX type lattice (E1). Identity is TEXT — this domain has no cooked data to stay compatible
/// with, so the trap `expr.h` records about positional identity does not have to be sprung.
enum VfxType : TypeId {
    Float = 0,
    Float2,
    Float3,
    Float4,
    Int,
    Bool,
    VfxTypeCount,
};

[[nodiscard]] const char* vfx_type_name(TypeId type) noexcept;
/// The type whose components match `name`, or `kInvalidType`.
[[nodiscard]] TypeId vfx_type_from_name(Name name) noexcept;

/// The VFX operation table (E2).
enum VfxOp : OpId {
    /// A literal. `inline_leaf`, so a folded value produces no statement at all.
    Constant = 0,
    /// A system parameter, by name. Uniform across the dispatch. An unexposed parameter never
    /// reaches this op — the lowering folds it to `Constant`.
    Parameter,
    /// A READ of a particle attribute. Varying, and the reason this domain is not the material
    /// domain: what it reads is a mutable cell, and the write that changes it is in the ordered
    /// list rather than in this DAG.
    Attribute,
    /// A per-dispatch value the host supplies: `dt`, `emitter_age`, `particle_index`,
    /// `spawn_index`.
    EmitterInput,
    /// A read through a data interface: `symbol` is `<interface>.<field>`. `MergeClass::Sample`, so
    /// the sample-deduplication switch can be turned off independently of interning.
    Sample,
    /// A draw from the effect's random stream. `MergeClass::Never`: two draws are two values, and
    /// interning them into one would silently correlate what an author wrote as independent.
    Random,
    /// A curve lookup by name: `(t)`.
    Curve,
    /// Value noise: `(position)`.
    Noise,

    Add,
    Sub,
    Mul,
    Div,
    Min,
    Max,
    Dot,
    Cross,
    Length,
    Normalize,
    Sin,
    Cos,
    Pow,
    Saturate,
    Lerp,
    Select,
    Less,
    Greater,
    LogicalAnd,
    LogicalOr,
    LogicalNot,
    MakeVec3,
    MakeVec4,
    /// Extract one component: `value.mask`, with the index in `Immediate::mask`.
    Swizzle,

    // --- Collision. Appended, so every op above keeps its number and every cook key its value. ---
    //
    // A contact is a QUERY and a response is ARITHMETIC over it, and the split is what lets one
    // `vfx.collide.*` node have six outputs without the ordered write list learning anything new:
    // each output is a pure expression over one shared `Contact`, which common-subexpression
    // elimination keeps to one query however many outputs an author wires. See collision.h.

    /// `(position, radius, shape_a, shape_b)` -> `float4(normal, separation)`. Which collider is
    /// `ColliderShape` in `Immediate::mask`; `symbol` spells it for a reader. Separation is the
    /// signed distance from the particle's surface to the collider's, negative when they overlap,
    /// and `kNoContact` when the collider is absent, off screen or turned off by the budget.
    Contact,
    /// `(velocity, contact, response)` -> the velocity after the contact. `response` is
    /// `float4(restitution, friction, stick, rest_speed)`.
    Bounce,
    /// `(position, contact)` -> the position pushed back out to the collider's surface.
    Resolve,
    /// `(velocity, contact)` -> the speed the particle was closing on the surface at, along its
    /// normal, while it touches it; zero otherwise.
    Impact,
    /// An event channel's contents, read by the emitter that consumes it: `symbol` is the channel,
    /// `Immediate::mask` is `EventField`. CPU path only; see `EventField`.
    EventRead,

    VfxOpCount,
};

/// Which collider a `Contact` queries. The order is the `Immediate::mask` a cooked kernel stores,
/// so it is append-only for the reason `VfxOp` is.
enum class ColliderShape : u32 {
    /// `shape_a = float4(normal, offset)`: the half-space `dot(normal, p) < offset` is solid.
    Plane = 0,
    /// `shape_a = float4(centre, radius)`.
    Sphere,
    /// An axis-aligned box: `shape_a = float4(centre, 0)`, `shape_b = float4(half_extents, 0)`.
    Box,
    /// `shape_a = float4(end_a, radius)`, `shape_b = float4(end_b, 0)`.
    Capsule,
    /// The scene signed distance field — the GI clipmaps on the device. World space, so it sees
    /// geometry off screen and behind the camera. `shape_a.x` is the gradient's sample step.
    Sdf,
    /// The depth buffer, in screen space: cheap, and blind to everything the camera does not see.
    /// `shape_a.x` is how thick a surface is taken to be behind its depth.
    Depth,
    Count,
};

[[nodiscard]] const char* collider_shape_name(ColliderShape shape) noexcept;

/// What an `EventRead` returns.
enum class EventField : u32 {
    /// Events on the channel this emitter has not consumed yet, this frame. A Spawn stage that
    /// writes it to `spawn_count` spawns one particle an event.
    Count = 0,
    /// The payload of the event this particle was spawned for: the one at `spawn_index` past the
    /// emitter's cursor. Read by Initialise.
    Payload,
};

/// The most event channels one emitter's Spawn stage may spawn from. The runtime keeps one cursor a
/// channel per emitter on the stack of a sub-step, and the compiler refuses a fifth rather than let
/// it be read as though nothing had been consumed from it.
inline constexpr u32 kMaxConsumedChannels = 4;

/// How close to a collider's surface a particle counts as touching it, in metres. One
/// millimetre: a resting particle is pushed back to separation zero every step and must still read
/// as touching, and a particle that has bounced clear moves further than this in one step.
inline constexpr f32 kContactSkin = 1.0e-3F;
/// The separation a `Contact` reports when there is nothing to touch.
inline constexpr f32 kNoContact = 1.0e6F;
/// The sample step an `sdf` contact takes its gradient over when its node leaves it zero, in
/// metres.
inline constexpr f32 kDefaultSdfStep = 0.05F;
/// How thick a `depth` surface is taken to be when its node leaves it zero, in metres.
inline constexpr f32 kDefaultDepthThickness = 0.5F;

[[nodiscard]] const char* vfx_op_name(OpId op) noexcept;

/// The declared roots (E3). See the note at the top of this file.
inline constexpr u32 kMaxKernelWrites = 16;
inline constexpr u32 kKillRoot = kMaxKernelWrites;
inline constexpr u32 kSpawnRoot = kMaxKernelWrites + 1;
inline constexpr u32 kVfxRootCount = kMaxKernelWrites + 2;

/// The one domain every VFX kernel is built in.
[[nodiscard]] const Domain& vfx_domain() noexcept;

/// How many scalar components a VFX type has. Zero for a type this lattice does not have.
[[nodiscard]] u32 vfx_type_components(TypeId type) noexcept;

// --- The arithmetic, spelled ONCE --------------------------------------------------------------
//
// The compiler's constant folder and the CPU executor must agree bit for bit: a value folded at
// cook time and the same value computed at run time have to be the same number, or an effect
// changes when a parameter is exposed. So the elementwise arithmetic is one function that both
// call, rather than two switch statements that look alike.

[[nodiscard]] f32 immediate_component(const Immediate& value, u32 index) noexcept;
void set_immediate_component(Immediate& value, u32 index, f32 component) noexcept;

/// Elementwise `op` over two immediates, broadcasting either side when it is a scalar. False when
/// the operation is not elementwise or the divisor is zero — a fold that would have to invent a
/// value declines instead.
[[nodiscard]] bool fold_elementwise_public(OpId op, const Immediate& a, const Immediate& b,
                                           u32 components, bool a_scalar, bool b_scalar,
                                           Immediate& out) noexcept;

/// One attribute write, in authored order. `root_slot` indexes the module's root table, so the
/// write survives every rebuild the optimiser performs: a rebuild renumbers `NodeId`s and carries
/// the roots across, which is precisely why the write list holds a slot and not a node.
struct KernelWrite {
    Name attribute;
    u32 root_slot = 0;
    /// The authoring node that produced this write, for a node-precise diagnostic.
    graph::NodeKey origin = graph::kInvalidNodeKey;
};

/// One event a kernel raises, in authored order. `root_slot` indexes the same fixed root table the
/// writes use, so a kernel's writes and its events share `kMaxKernelWrites` between them — a bound
/// that is reported (`ErrorCode::OutOfRange`, naming the kernel) rather than truncated.
struct KernelEvent {
    Name channel;
    u32 root_slot = 0;
    graph::NodeKey origin = graph::kInvalidNodeKey;
    /// The root the raise's `payload` input landed in, or `kNoPayloadRoot` when the predicate is
    /// the payload. It shares the same table, so a payload costs the kernel one more root.
    u32 payload_root = 0xFFFFFFFFU;
};

/// `KernelEvent::payload_root` for a raise whose predicate is its payload.
inline constexpr u32 kNoPayloadRoot = 0xFFFFFFFFU;

/// The compiled per-invocation program the CPU path steps through. Not a graph interpreter — see
/// the note at the top of this file.
struct KernelStep {
    OpId op = Constant;
    /// Operand slots. `0xFFFF` for an operand this op does not take.
    ///
    /// FOUR OF THEM, because `kMaxOperands` is four and `make_float4` uses all of them. Three was
    /// the first version of this structure and it silently dropped the fourth: every particle's
    /// colour came back with an alpha of zero, the frame drew two thousand fully transparent
    /// sprites, and every assertion about counts still passed. It was found by looking at the
    /// picture, which is the whole argument for this milestone photographing what it builds.
    u16 a = 0xFFFFU;
    u16 b = 0xFFFFU;
    u16 c = 0xFFFFU;
    u16 d = 0xFFFFU;
    u16 dst = 0;
    TypeId type = Float;
    Immediate value;
    /// A parameter's name, an attribute's name, `<interface>.<field>`, a curve's name.
    Name symbol;
};

/// Which stages a kernel covers. A bit per `Stage`, so a fused Initialise+Update kernel reports
/// both and `dispatches` counts one.
using StageMask = u8;

[[nodiscard]] constexpr StageMask stage_bit(Stage stage) noexcept {
    return static_cast<StageMask>(1U << static_cast<u32>(stage));
}

/// ONE DISPATCH. The expression module, the ordered write list, and the flat program the CPU path
/// runs.
class VfxKernel {
public:
    VfxKernel(Allocator& allocator, const Domain& domain) noexcept;

    VfxKernel(const VfxKernel&) = delete;
    VfxKernel& operator=(const VfxKernel&) = delete;
    VfxKernel(VfxKernel&&) noexcept = default;
    VfxKernel& operator=(VfxKernel&&) noexcept = default;

    [[nodiscard]] Name name() const noexcept { return name_; }
    [[nodiscard]] StageMask stages() const noexcept { return stages_; }
    [[nodiscard]] bool covers(Stage stage) const noexcept {
        return (stages_ & stage_bit(stage)) != 0;
    }
    [[nodiscard]] const Module& expressions() const noexcept { return expressions_; }
    [[nodiscard]] Span<const KernelWrite> writes() const noexcept { return writes_.span(); }
    /// The event raises, in authored order. A raise whose predicate evaluates non-zero puts one
    /// record on its channel; the channel's own bounds decide whether it survives.
    [[nodiscard]] Span<const KernelEvent> events() const noexcept { return events_.span(); }
    /// The slot each raise's predicate lands in, parallel to `events()`.
    [[nodiscard]] Span<const u16> event_slots() const noexcept { return event_slots_.span(); }
    /// The slot each raise's payload lands in, parallel to `events()`; `0xFFFF` where the
    /// predicate is the payload.
    [[nodiscard]] Span<const u16> event_payload_slots() const noexcept {
        return event_payload_slots_.span();
    }
    [[nodiscard]] Span<const KernelStep> program() const noexcept { return program_.span(); }
    [[nodiscard]] u32 slot_count() const noexcept { return slots_; }
    /// The slot each write's value lands in, parallel to `writes()`.
    [[nodiscard]] Span<const u16> write_slots() const noexcept { return write_slots_.span(); }
    /// The slot the kill predicate lands in, or `0xFFFF`.
    [[nodiscard]] u16 kill_slot() const noexcept { return kill_slot_; }
    /// The slot the spawn count lands in, or `0xFFFF`.
    [[nodiscard]] u16 spawn_slot() const noexcept { return spawn_slot_; }

    /// `kIrVersion`, the kernel's stages, and the module's own digest. The cook key is built from
    /// this rather than from a second hash of the same content.
    [[nodiscard]] u64 digest() const noexcept { return digest_; }

    [[nodiscard]] Allocator& allocator() const noexcept { return writes_.allocator(); }

private:
    friend class KernelAccess;

    Name name_;
    StageMask stages_ = 0;
    Module expressions_;
    Array<KernelWrite> writes_;
    Array<KernelEvent> events_;
    Array<KernelStep> program_;
    Array<u16> write_slots_;
    Array<u16> event_slots_;
    Array<u16> event_payload_slots_;
    u32 slots_ = 0;
    u16 kill_slot_ = 0xFFFFU;
    u16 spawn_slot_ = 0xFFFFU;
    u64 digest_ = 0;
};

/// The per-dispatch inputs a kernel reads through `VfxOp::Emitter`. Spelled once so the compiler
/// and the executor cannot disagree about the names.
namespace input {
inline constexpr const char* kDeltaTime = "dt";
inline constexpr const char* kEmitterAge = "emitter_age";
inline constexpr const char* kParticleIndex = "particle_index";
inline constexpr const char* kSpawnIndex = "spawn_index";
inline constexpr const char* kNormalisedAge = "normalised_age";
}  // namespace input

}  // namespace cy::vfx
