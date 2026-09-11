#pragma once
// THE ATTRIBUTE LAYOUT IS AN OUTPUT OF COMPILATION, NOT A STRUCT. M8.c task 2.2.
//
// ================================================================================================
// WHAT THE REQUIREMENT ACTUALLY ASKS FOR
// ================================================================================================
//
// `vfx-system`, "Compiler-derived attribute layout", in three sentences and all three are checked
// here:
//
//   * "Particle storage SHALL be structure-of-arrays, and the set of attribute arrays SHALL be
//     DERIVED BY THE COMPILER from the attributes the graphs actually read or write."
//   * "Attributes never referenced by any stage of an emitter SHALL NOT be allocated."
//   * "The compiler SHALL additionally select each attribute's storage precision from declared
//     ranges and usage WHERE A REDUCED PRECISION IS PROVABLY SUFFICIENT, with an explicit
//     authoring override."
//
// So there is no `struct Particle` anywhere in this module, and there cannot be one: the set of
// arrays is a function of the graphs. `AttributeLayout` below is that function's result.
//
// ================================================================================================
// "PROVABLY SUFFICIENT" IS A COMPUTATION, AND THIS IS THE COMPUTATION
// ================================================================================================
//
// An author declares a range and a TOLERANCE — the largest absolute error they accept in that
// attribute — and the compiler picks the smallest encoding whose worst representable step over the
// declared range is no larger than it:
//
//   `Unorm8`    range must be within [0, 1]. Step = (max - min) / 255.
//   `Snorm16`   range must be within [-1, 1]. Step = (max - min) / 32767.
//   `Float16`   step = |bound| * 2^-10, where `bound` is the larger magnitude of the range.
//   `Float32`   step = |bound| * 2^-23. The fallback, and the answer whenever nothing else fits.
//
// **A tolerance of zero is the default, and it selects `Float32` every time.** That is the whole
// safety property: an author who says nothing gets nothing quantised, and a colour that was
// authored at eight bits per channel is eight bits per channel because somebody wrote down that
// 1/255 was acceptable. A compiler that guessed from the attribute's NAME would quantise
// `position` in the one effect that called its opacity `position`.
//
// ================================================================================================
// THE LAYOUT IS REPORTED, WHICH IS ALSO A REQUIREMENT
// ================================================================================================
//
// "WHEN an effect is cooked THEN its derived attribute layout, per-particle byte size, and
// resulting MAXIMUM POPULATION FOR A GIVEN MEMORY BUDGET SHALL be reported to the author."
// `max_population()` is that third number, and it is a method rather than a field because the
// budget is the caller's.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/vfx/asset.h>
#include <cy/vfx/ir.h>

namespace cy::vfx {

/// One derived attribute array. STRUCTURE OF ARRAYS: `offset` is the start of this attribute's
/// whole array within the pool's block for the emitter, not a field offset within a particle.
struct AttributeSlot {
    Name name;
    TypeId type = Float;
    Precision precision = Precision::Float32;
    u32 components = 1;
    /// Bytes one particle's value occupies: `components * precision_bytes(precision)`.
    u32 stride = 4;
    /// Byte offset of this attribute's array within the emitter's block, for a given capacity.
    /// Filled by `AttributeLayout::resolve_offsets`.
    u32 array_offset = 0;
    bool read = false;
    bool written = false;
    /// The attribute was declared or referenced and then found dead by liveness analysis. Kept in
    /// the report and NOT allocated, so "what did the compiler drop" is answerable.
    bool elided = false;
};

/// The derived layout of one emitter's particle storage.
class AttributeLayout {
public:
    explicit AttributeLayout(Allocator& allocator) noexcept : slots_(allocator) {}

    AttributeLayout(const AttributeLayout&) = delete;
    AttributeLayout& operator=(const AttributeLayout&) = delete;
    AttributeLayout(AttributeLayout&&) noexcept = default;
    AttributeLayout& operator=(AttributeLayout&&) noexcept = default;

    /// Note that `attribute` is read, written, or both. Creates the slot on first mention.
    [[nodiscard]] Status touch(Name attribute, TypeId type, bool read, bool written) noexcept;

    /// Apply an author's declaration — range, tolerance, override — to a slot that exists.
    /// A declaration for an attribute no stage mentions is NOT an error: it is a declaration about
    /// an attribute that is not allocated, and `elided_attributes()` counts it.
    [[nodiscard]] Status declare(const AttributeDecl& decl) noexcept;

    /// Mark every slot that is written and never read — by any stage of this emitter, including
    /// `Render` — as elided. This is the attribute liveness analysis `vfx-system` names.
    void eliminate_dead(Span<const Name> live_outputs) noexcept;

    /// Choose each live slot's precision and assign array offsets for `capacity` particles.
    [[nodiscard]] Status resolve(u32 capacity) noexcept;

    [[nodiscard]] Span<const AttributeSlot> slots() const noexcept { return slots_.span(); }
    [[nodiscard]] const AttributeSlot* find(Name attribute) const noexcept;
    /// Live slots only.
    [[nodiscard]] u32 allocated_attributes() const noexcept;
    [[nodiscard]] u32 elided_attributes() const noexcept;
    /// The sum of the live slots' strides. This is the number an author is shown.
    [[nodiscard]] u32 bytes_per_particle() const noexcept { return bytes_per_particle_; }
    /// Bytes the whole emitter block occupies at the capacity `resolve` was given.
    [[nodiscard]] u64 block_bytes() const noexcept { return block_bytes_; }
    /// "resulting maximum population for a given memory budget".
    [[nodiscard]] u32 max_population(u64 memory_budget_bytes) const noexcept;
    [[nodiscard]] bool resolved() const noexcept { return resolved_; }

    /// A content hash over the live slots in name order: the layout's identity, and the value a
    /// hot reload compares to decide whether running instances must be RESTARTED rather than
    /// migrated. `vfx-system` requires exactly that restart, and this is what makes it decidable.
    [[nodiscard]] u64 digest() const noexcept { return digest_; }

    [[nodiscard]] Allocator& allocator() const noexcept { return slots_.allocator(); }

private:
    Array<AttributeSlot> slots_;
    u32 bytes_per_particle_ = 0;
    u64 block_bytes_ = 0;
    u64 digest_ = 0;
    bool resolved_ = false;
};

/// The precision the rules at the top of this file select for one declaration. Exposed because it
/// is the whole of "provably sufficient" and a test that could not call it would be testing the
/// compiler's plumbing instead.
[[nodiscard]] Precision select_precision(const AttributeDecl& decl, u32 components) noexcept;

// --- Reading and writing one attribute at the precision the compiler chose -----------------------
//
// THE ONE PLACE THE DERIVED LAYOUT BECOMES OBSERVABLE ON THE CPU PATH. An attribute the compiler
// put at `Unorm8` reads back quantised through these two functions, exactly as it would through the
// generated Slang's `cyVfxUnpackUnorm8` — so the layout is a property of the simulation rather than
// a line in a cook report. `emit_slang.cpp` generates the shader-side spelling of the same four
// conversions, and the two are checked against each other in `test_vfx_compiler.cpp`.

/// Read component `component` of `particle`'s value of `slot`, as f32 whatever it is stored as.
[[nodiscard]] f32 load_component(Span<const u8> block, const AttributeSlot& slot, u32 particle,
                                 u32 component) noexcept;

/// Write it, quantising to the slot's precision. Out-of-range indices are ignored rather than
/// trapping: a kernel that walked off its block is a defect the pool's bounds report, and a crash
/// here would report it in the wrong place.
void store_component(Span<u8> block, const AttributeSlot& slot, u32 particle, u32 component,
                     f32 value) noexcept;

/// The worst absolute step of an encoding over a range. Zero when the encoding cannot represent the
/// range at all, which is how `select_precision` rejects `Unorm8` for a signed attribute.
[[nodiscard]] f32 precision_step(Precision precision, f32 minimum, f32 maximum) noexcept;

}  // namespace cy::vfx
