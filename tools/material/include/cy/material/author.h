#ifndef CY_MATERIAL_AUTHOR_H
#define CY_MATERIAL_AUTHOR_H
// Reading the editor's material canvas. M11.c task 6.1a. See author.cpp for why it exists.

#include <cy/core/memory/array.h>
#include <cy/rendering/material/ir.h>

#include <string_view>

namespace cy::material {

/// The sidecar's version.
inline constexpr u32 kInfoVersion = 1;

/// Write the sidecar a frame reads to check the parameter block it is about to upload.
///
/// The parameters and the textures IN THE MODULE'S OWN DECLARATION ORDER, which is what decides the
/// generated `CyMaterialParams` layout. See author.cpp.
[[nodiscard]] Status write_material_info(const rendering::material::Module& module, u64 cook_key,
                                         std::string_view entry_point, Array<char>& out) noexcept;

}  // namespace cy::material

/// `cy_material author`. Declared here rather than in `main.cpp` so the subcommand is a unit a test
/// could drive, and so `main.cpp` stays a dispatcher.
[[nodiscard]] int cy_material_author(int argc, char** argv);

#endif  // CY_MATERIAL_AUTHOR_H
