#ifndef CY_SHADER_SPIRV_H
#define CY_SHADER_SPIRV_H
// SPIR-V as the interchange form: validation of its shape, and reflection out of it. Tasks 3.1 and
// 3.3.
//
// --- WHY THE ENGINE PARSES SPIR-V ITSELF -----------------------------------------------------------
//
// It looks like the one place a dependency would obviously be right — SPIRV-Reflect exists and is
// small. Three reasons it is not:
//
//   * **Reflection is a cook-time and load-time input to correctness, not a convenience.** Task 3.3
//     says bindings derive from reflection rather than a hand-maintained table; that makes the
//     reflector part of the binding contract, and a contract implemented by a library the engine
//     does not control is one whose behaviour changes under it.
//   * **The output shape is the engine's, not the reflector's.** Every reflection library returns
//     its own structs, so a wrapper would be written anyway; what is saved is the traversal, and the
//     traversal is the part below — a linear walk over a word stream with no allocation per
//     instruction.
//   * **It has to work with no device and no Vulkan headers.** `Reflection` is derived and validated
//     in CI on machines with neither, which is what makes the null-backend path meaningful.
//
// This is a *reflector*, emphatically not a validator and not an optimiser. It trusts the module's
// structure and reports what it finds; `spirv-val` is the validator and Slang is the optimiser, and
// `thirdparty-dependencies` is clear that the engine writes neither.
//
// --- WHAT IT UNDERSTANDS ---------------------------------------------------------------------------
//
// One linear pass collects names, decorations, types, constants, variables and entry points; a
// second attributes each variable to the entry points whose interface list names it. SPIR-V 1.4 and
// later put *every* global a function reaches into that interface list, which is what makes a
// per-stage `StageMask` derivable at all — before 1.4 the list held only Input and Output, and a
// module built to that version reports stages only for its vertex attributes. Slang emits 1.5.
//
// Anything it does not recognise it steps over by word count, which is why an unknown opcode from a
// newer SPIR-V is not an error: the format is self-describing in exactly the way that matters.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/shader/reflection.h>

namespace cy::shader {

/// The first word of every SPIR-V module.
inline constexpr u32 kSpirvMagic = 0x0723'0203U;

/// Words in the module header, before the first instruction.
inline constexpr u32 kSpirvHeaderWords = 5;

/// What the header says, without parsing a single instruction.
struct SpirvHeader {
    u32 version = 0;
    u32 generator = 0;
    /// One past the largest result id. Every id table is sized from it, which is why a module
    /// claiming an absurd bound is rejected before anything is allocated.
    u32 id_bound = 0;

    [[nodiscard]] u32 version_major() const noexcept { return (version >> 16) & 0xFFU; }
    [[nodiscard]] u32 version_minor() const noexcept { return (version >> 8) & 0xFFU; }
};

/// Read and check the header. Rejects a wrong magic, a byte-swapped module (the engine never
/// produces one and converting it would hide a build that targets the wrong endianness), a truncated
/// header, and an id bound large enough to be a length mistake rather than a shader.
[[nodiscard]] Expected<SpirvHeader, Error> spirv_header(Span<const u32> words) noexcept;

/// Reflect a whole module.
///
/// The result is canonicalised — see `Reflection::canonicalise()` — so two runs over the same bytes
/// produce byte-identical reflection, which is what lets a layout hash be a cache key.
///
/// Fails on a module that is structurally impossible to reflect: a bad header, an instruction whose
/// word count is zero (which would loop forever), an instruction running past the end. It does not
/// fail on a module that is merely *wrong* — a binding with no descriptor set decoration is reported
/// as set 0, because that is what the driver will do with it, and the convention validator is where
/// that becomes a diagnostic.
[[nodiscard]] Expected<Reflection, Error> reflect_spirv(Span<const u32> words,
                                                        Allocator& allocator) noexcept;

/// Count instructions without building reflection. Used by the build report, which wants the number
/// for every variant and the full reflection for none of them.
[[nodiscard]] Expected<u32, Error> spirv_instruction_count(Span<const u32> words) noexcept;

/// The entry-point names a module declares, in declaration order, with their stages.
///
/// **The name here is not the name in the Slang source.** Slang renames an entry point to `main` in
/// the SPIR-V it emits, so a pipeline's `pName` must come from reflection rather than from the
/// request that produced the module. Getting this wrong produces a pipeline creation failure whose
/// message names neither the shader nor the entry point, so it is worth a function of its own.
[[nodiscard]] Expected<u32, Error> spirv_entry_points(Span<const u32> words,
                                                      Array<EntryPointReflection>& out) noexcept;

}  // namespace cy::shader

#endif  // CY_SHADER_SPIRV_H
