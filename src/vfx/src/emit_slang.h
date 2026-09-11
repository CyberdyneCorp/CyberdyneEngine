#pragma once
// The Slang back end of the VFX compiler. M8.c task 2.1.
//
// Private to the compiler target: `compile.h`'s `emit_prelude` and `assemble_translation_unit` are
// the public surface, and this is the step between them — one generated program per kernel, filled
// into `CompiledEmitter::sources()` while the emitter is being compiled.

#include <cy/core/base/expected.h>
#include <cy/vfx/compile.h>

namespace cy::vfx {

/// Emit one Slang program per kernel into `emitter`'s source list. Called by `compile_system` after
/// the layout is resolved, because a store's spelling depends on the precision the layout chose.
[[nodiscard]] Status emit_emitter_sources(CompiledEmitter& emitter,
                                          Span<const ParameterDecl> parameters) noexcept;

}  // namespace cy::vfx
