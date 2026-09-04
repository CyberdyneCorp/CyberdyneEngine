#pragma once
// The Slang front end: Slang source in, SPIR-V out. Task 3.1.
//
// `shader-system` — "Slang as the authoring language": engine and user shaders are written in
// Slang and compiled to SPIR-V. `thirdparty-dependencies` is the other half of the same decision:
// the engine integrates a shader toolchain and does not author a shading language.
//
// THE MODULE IS EXCLUDED FROM THE LINK ENTIRELY WHEN CY_SHADER_SLANG IS OFF, so a shipping build
// has no Slang code in it rather than Slang code that is never called — which is what
// `shader-system`'s "Shipping build compiles no shaders from source" scenario requires. Everything
// a shipped game needs is already in the shader library and reaches the device through the SPIR-V
// passthrough in cy/backends/shader/compiler.h.
//
// NO SLANG TYPE APPEARS IN THIS HEADER, and none appears anywhere outside
// src/backends/shader/slang/src/. That is the same rule that keeps Vulkan inside
// src/backends/rhi/vulkan/, and tools/layercheck/layercheck.py's `gpuapi` check enforces it.

#include <cy/backends/shader/compiler.h>

namespace cy::shader::slang {

/// Register the Slang front end. It registers itself when this module is part of the link; this
/// exists so a host can make it a statement rather than a link-order property. Idempotent by name.
Status register_slang_backend() noexcept;

/// Construct one directly, bypassing the registry. `create_compiler(allocator, "slang", ...)` is
/// the ordinary path; this is for a tool that wants the front end and nothing else.
[[nodiscard]] Expected<ShaderCompiler*, Error> create_slang_compiler(Allocator& allocator) noexcept;
void destroy_slang_compiler(Allocator& allocator, ShaderCompiler* compiler) noexcept;

/// Whether the Slang runtime could be initialised. False when the library is present but the core
/// module could not be loaded, which is the failure a stripped install produces.
[[nodiscard]] bool slang_available() noexcept;

}  // namespace cy::shader::slang
