#ifndef CY_SHADER_GLOBALS_H
#define CY_SHADER_GLOBALS_H
// Global shader parameters: project-wide values every shader can read. Task 3.6.
//
// `shader-system` — "Global shader parameters": named, project-wide values (wind, time of day,
// gameplay state) settable at runtime and readable by any shader, stored in a global uniform
// buffer, with **no per-material update needed**. One update, and every shader referencing the
// parameter observes the new value the next frame, with no material or pipeline changes.
//
// --- WHERE THE LAYOUT COMES FROM
// ------------------------------------------------------------------
//
// The obvious implementation gives each parameter an offset chosen by the C++ side and a matching
// `float` at that offset in a hand-written Slang struct. That is precisely the hand-maintained
// table task 3.3 exists to abolish, one subsystem over: the struct and the table drift, and the
// symptom is wind blowing at the time of day.
//
// So the layout is **derived from the declaration order**, computed once by `finalise()`, and the
// Slang side reads it from generated source rather than from a hand-written struct — the generator
// is `emit_slang_declaration()` below, and its output goes through `SourceStore::add_generated()`
// like any other generated module. That is task 3.8's seam serving its first real consumer, months
// before the material compiler arrives, which is the cheapest possible way to find out whether the
// seam is the right shape.
//
// --- THE PACKING RULE
// ------------------------------------------------------------------------------
//
// std140, because the block is a uniform buffer and that is what a uniform buffer's layout is:
// scalars align to 4, `float2` to 8, `float3` and `float4` to 16, and a `float3` does not straddle
// a 16-byte boundary. `finalise()` sorts by decreasing alignment before assigning offsets, so a
// declaration order that would waste half the block does not; the sort is stable on the declaration
// index, so the layout is deterministic and the generated Slang matches it exactly.
//
// --- WHAT "THE NEXT FRAME" MEANS
// -------------------------------------------------------------------
//
// `set()` writes into the CPU-side block and bumps `version()`. The renderer uploads the block once
// per frame when the version has changed. There is no per-shader and no per-material path, which is
// the requirement; there is also no way for a shader to observe a *partial* update, because the
// upload takes the whole block.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/shader/shader.h>

namespace cy::shader {

/// The types a global parameter may have. Deliberately small: a global is a project-wide scalar or
/// vector, and a global matrix or texture is a view or material parameter that has escaped.
enum class GlobalType : u8 {
    Float = 0,
    Float2 = 1,
    Float3 = 2,
    Float4 = 3,
    Int = 4,
    Uint = 5,
    Bool = 6,
};

const char* global_type_name(GlobalType type) noexcept;
/// The Slang spelling: `float`, `float3`, `uint`. What the generated declaration writes.
const char* global_type_slang(GlobalType type) noexcept;
[[nodiscard]] u32 global_type_size(GlobalType type) noexcept;
/// std140 alignment: 4, 8 or 16.
[[nodiscard]] u32 global_type_alignment(GlobalType type) noexcept;

/// A handle onto a declared parameter. Resolving a name to one costs a scan; doing it per frame is
/// what this exists to prevent.
struct GlobalId {
    u32 value = 0;

    [[nodiscard]] bool valid() const noexcept { return value != 0; }
    friend bool operator==(GlobalId a, GlobalId b) noexcept { return a.value == b.value; }
    friend bool operator!=(GlobalId a, GlobalId b) noexcept { return a.value != b.value; }
};

/// One parameter, after the layout has been computed.
struct GlobalParameter {
    ShortName name;
    GlobalType type = GlobalType::Float;
    /// Byte offset in the block. Meaningless before `finalise()`.
    u32 offset = 0;
};

/// The largest a global block may be. Vulkan guarantees a 16 KiB uniform buffer range; the block is
/// bounded well below that because it is bound on **every** pipeline, and a project that wants a
/// kilobyte of globals wants a storage buffer of its own.
inline constexpr u32 kMaxGlobalBlockBytes = 1024;

/// The block's name in Slang and in reflection. Reserved: `reflection.h` pins it to set 0.
inline constexpr const char* kGlobalBlockName = "cy_globals";

/// The declared globals, their layout, and their current values.
///
/// Not thread-safe. `set()` is called from gameplay and the upload is taken at the frame boundary;
/// a lock here would only make a torn read across two parameters look safe when it is not, and the
/// engine's answer to that is the commit boundary, not a mutex.
class GlobalParameters {
public:
    explicit GlobalParameters(Allocator& allocator) noexcept;

    GlobalParameters(const GlobalParameters&) = delete;
    GlobalParameters& operator=(const GlobalParameters&) = delete;

    /// Declare a parameter. Only before `finalise()`: a global that appears after the block's
    /// layout has been published would change the offsets every compiled shader was built against.
    [[nodiscard]] Expected<GlobalId, Error> declare(const char* name, GlobalType type) noexcept;

    /// Compute the layout and lock the declaration. Idempotent.
    ///
    /// Fails when the block would exceed `kMaxGlobalBlockBytes`, naming the parameter that crossed
    /// it — because "the global block is too big" without a name is a message that sends someone to
    /// read every declaration in the project.
    [[nodiscard]] Status finalise() noexcept;
    [[nodiscard]] bool finalised() const noexcept { return finalised_; }

    [[nodiscard]] GlobalId find(const char* name) const noexcept;
    [[nodiscard]] u32 count() const noexcept { return static_cast<u32>(parameters_.size()); }
    [[nodiscard]] GlobalParameter parameter(GlobalId id) const noexcept;
    /// Iterate in layout order — the order `emit_slang_declaration()` writes and the order the
    /// block is packed in. Deterministic, whatever order the declarations arrived in.
    [[nodiscard]] GlobalParameter at(u32 index) const noexcept;

    /// Write a value. The type must match the declaration; a mismatch is a programmer error and
    /// trips `CY_ASSERT` rather than silently reinterpreting four bytes.
    void set_float(GlobalId id, f32 value) noexcept;
    void set_float2(GlobalId id, const f32 (&value)[2]) noexcept;
    void set_float3(GlobalId id, const f32 (&value)[3]) noexcept;
    void set_float4(GlobalId id, const f32 (&value)[4]) noexcept;
    void set_int(GlobalId id, i32 value) noexcept;
    void set_uint(GlobalId id, u32 value) noexcept;
    void set_bool(GlobalId id, bool value) noexcept;

    [[nodiscard]] f32 get_float(GlobalId id) const noexcept;
    [[nodiscard]] u32 get_uint(GlobalId id) const noexcept;

    /// The packed block. Uploaded whole, once per frame, when `version()` has changed.
    [[nodiscard]] Span<const u8> block() const noexcept { return block_.span(); }
    [[nodiscard]] u32 block_size() const noexcept { return static_cast<u32>(block_.size()); }
    /// Incremented by every `set_*`. The renderer compares it against what it last uploaded.
    [[nodiscard]] u64 version() const noexcept { return version_; }

    /// Emit the Slang declaration of the block, into `out`.
    ///
    /// **This is the generated source that keeps the C++ layout and the shader struct in step.** It
    /// goes to `SourceStore::add_generated("cy.globals", ...)` and is imported by every shader that
    /// reads a global. Nothing hand-writes the struct, so nothing can disagree with the packing.
    [[nodiscard]] Status emit_slang_declaration(Array<char>& out) const noexcept;

private:
    struct Declaration {
        ShortName name;
        GlobalType type = GlobalType::Float;
        u32 offset = 0;
        /// The order `declare()` was called in. The stable tie-break of the layout sort, and what
        /// makes the layout reproducible across runs.
        u32 sequence = 0;
    };

    [[nodiscard]] Declaration& at_id(GlobalId id) noexcept;
    [[nodiscard]] const Declaration& at_id(GlobalId id) const noexcept;
    void write(GlobalId id, GlobalType type, const void* data, u32 size) noexcept;

    Array<Declaration> parameters_;
    Array<u8> block_;
    u64 version_ = 0;
    bool finalised_ = false;
};

}  // namespace cy::shader

#endif  // CY_SHADER_GLOBALS_H
