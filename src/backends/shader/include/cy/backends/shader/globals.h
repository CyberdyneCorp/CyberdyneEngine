#pragma once
// Global shader parameters: one buffer, every shader. Task 3.6.
//
// `shader-system` — "Global shader parameters": "named project-wide values (wind, time of day,
// gameplay state) settable at runtime and readable by any shader — stored in a global uniform
// buffer, with no per-material update needed", and its scenario: "when a global parameter is set,
// every shader referencing it SHALL observe the new value the next frame, with no material or
// pipeline changes".
//
// THE REQUIREMENT IS ABOUT WHAT DOES *NOT* HAPPEN. Setting a global must not touch a material, must
// not invalidate a pipeline, and must not walk a list of shaders. So the block is one contiguous
// buffer at a fixed binding in descriptor set 0 (`reflection.h`'s convention: set 0 is per-frame),
// every shader that names a global reads it from there, and setting one is a memcpy into a byte
// range. `revision()` is the only thing the frame consults: it moved, so the buffer is uploaded.
//
// LAYOUT IS ASSIGNED, NOT DECLARED TWICE. Offsets follow the std140 rules the shading languages
// agree on — scalars at 4, two-component vectors at 8, three- and four-component vectors and
// matrix columns at 16 — because that is what a `cbuffer` in Slang produces and a layout the engine
// invented would differ from the shader that reads it. `validate_against()` then checks the block
// against the *reflected* binding rather than trusting the agreement, which is the same posture
// reflection.h takes about descriptor layouts and for the same reason.
//
// THE BLOCK IS FROZEN BEFORE IT IS USED. Declaring a parameter assigns an offset; a shader compiled
// against the block encodes those offsets. Declaring another one after a shader has been compiled
// would move nothing — offsets are append-only — but it would change the block's *layout hash*, and
// a mismatch there is a shader reading a buffer laid out differently from the one it was compiled
// against. `freeze()` makes the boundary explicit and `layout_hash()` is what a compiled artefact
// records so the mismatch is caught rather than rendered.

#include <cy/backends/shader/diagnostics.h>
#include <cy/backends/shader/reflection.h>
#include <cy/core/assets/hash.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>

namespace cy::shader {

/// The set and binding the global block occupies. Set 0 is the per-frame set by the convention in
/// reflection.h; binding 0 within it is reserved for this block so that every shader agrees without
/// anybody being asked.
inline constexpr u32 kGlobalsSet = kSetGlobal;
inline constexpr u32 kGlobalsBinding = 0;

/// The largest a global block may be. A uniform buffer this size is guaranteed by every device the
/// engine targets, and a project needing more has a storage buffer's worth of data rather than a
/// set of project-wide constants.
inline constexpr u32 kMaxGlobalBlockBytes = 16 * 1024;

enum class GlobalType : u8 {
    Float = 0,
    Vec2 = 1,
    Vec3 = 2,
    Vec4 = 3,
    Int = 4,
    UInt = 5,
    Bool = 6,
    Mat4 = 7,
};

const char* global_type_name(GlobalType type) noexcept;
/// Bytes the value occupies, ignoring alignment.
[[nodiscard]] u32 global_type_size(GlobalType type) noexcept;
/// The std140 alignment the value must start at.
[[nodiscard]] u32 global_type_alignment(GlobalType type) noexcept;

struct GlobalParameter {
    Name name;
    GlobalType type = GlobalType::Float;
    u32 offset = 0;
};

struct GlobalParameterId {
    u32 value = 0xFFFF'FFFFU;
    [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0xFFFF'FFFFU; }
};

/// The project's global parameters and their storage.
///
/// Not thread-safe. Globals are set from game code at a defined point in the frame — the same
/// commit boundary `core-determinism` already fixes — and a block that took a lock per set would be
/// paying for a race the frame structure already prevents.
class GlobalParameterBlock {
public:
    explicit GlobalParameterBlock(Allocator& allocator) noexcept;

    GlobalParameterBlock(const GlobalParameterBlock&) = delete;
    GlobalParameterBlock& operator=(const GlobalParameterBlock&) = delete;

    /// Declare a parameter and assign its offset. Fails on a duplicate name, on an unknown type, on
    /// exceeding `kMaxGlobalBlockBytes`, and after `freeze()`.
    [[nodiscard]] Expected<GlobalParameterId, Error> declare(Name name, GlobalType type) noexcept;

    /// No further declarations. Called once the project's globals are known and before anything is
    /// compiled against the block.
    void freeze() noexcept { frozen_ = true; }
    [[nodiscard]] bool frozen() const noexcept { return frozen_; }

    [[nodiscard]] GlobalParameterId find(Name name) const noexcept;
    [[nodiscard]] const GlobalParameter* parameter_at(GlobalParameterId id) const noexcept;
    [[nodiscard]] usize size() const noexcept { return parameters_.size(); }

    /// Set a value. The type must match the declaration — a mismatch is a programmer error the
    /// caller could have avoided by reading its own declaration, so it is reported rather than
    /// silently reinterpreted.
    [[nodiscard]] Status set_float(GlobalParameterId id, f32 value) noexcept;
    [[nodiscard]] Status set_int(GlobalParameterId id, i32 value) noexcept;
    [[nodiscard]] Status set_uint(GlobalParameterId id, u32 value) noexcept;
    [[nodiscard]] Status set_bool(GlobalParameterId id, bool value) noexcept;
    [[nodiscard]] Status set_vec(GlobalParameterId id, Span<const f32> components) noexcept;
    [[nodiscard]] Status set_mat4(GlobalParameterId id, Span<const f32> column_major) noexcept;

    /// The block's bytes, ready to be uploaded into the uniform buffer at (set 0, binding 0).
    [[nodiscard]] Span<const u8> data() const noexcept {
        return {storage_.data(), storage_.size()};
    }
    [[nodiscard]] u32 byte_size() const noexcept { return static_cast<u32>(storage_.size()); }

    /// Bumped by every successful set. The frame uploads when it differs from the value it last
    /// uploaded — one comparison per frame instead of a dirty flag per parameter.
    [[nodiscard]] u64 revision() const noexcept { return revision_; }

    /// A digest over the declared names, types and offsets. Recorded in a compiled artefact and
    /// compared at load: an artefact compiled against a different layout is refused rather than
    /// reading the wrong sixteen bytes.
    [[nodiscard]] assets::ContentHash layout_hash() const noexcept;

    /// Check the reflected binding at (set 0, binding 0) against this block. Reports a diagnostic
    /// and returns false when the shader does not declare it as a uniform buffer, which is the
    /// only structural disagreement the binary carries.
    [[nodiscard]] bool validate_against(const Reflection& reflection, DiagnosticLog& diagnostics,
                                        std::string_view shader_name) const noexcept;

    /// The Slang declaration of this block, appended to `out`, for a generator that has to emit a
    /// module importing it. Text rather than a header, because it is generated Slang like any other
    /// generated Slang and goes through `SourceRegistry::add_generated()`.
    [[nodiscard]] Status emit_slang_declaration(Array<char>& out) const noexcept;

private:
    [[nodiscard]] Status write(GlobalParameterId id, GlobalType expected, const void* data,
                               u32 size) noexcept;

    Array<GlobalParameter> parameters_;
    Array<u8> storage_;
    u64 revision_ = 0;
    bool frozen_ = false;
};

}  // namespace cy::shader
