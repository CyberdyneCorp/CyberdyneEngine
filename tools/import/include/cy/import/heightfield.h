// SPDX-License-Identifier: MIT
#pragma once
// Versioned heightfield ingestion for CyberTerrain. The source is deliberately raw and explicit:
// a raw grid does not carry dimensions, signedness, units, range, or tiling, so the importer
// refuses to infer any of them from byte count or file name.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/import/importer.h>

namespace cy::import {

/// Header at the front of every cooked terrain heightfield payload.
struct CookedHeightfield {
    static constexpr u32 kVersion = 1;
    static constexpr u32 kHeaderBytes = 40;

    u32 width = 0;
    u32 height = 0;
    u32 tile_quads = 0;
    u32 tiles_x = 0;
    u32 tiles_z = 0;
    f32 sample_metres = 0.0F;
    f32 height_min_metres = 0.0F;
    f32 height_max_metres = 0.0F;
};

/// Read the versioned header from a cooked payload.
[[nodiscard]] Expected<CookedHeightfield, Error> read_cooked_heightfield(
    Span<const u8> payload) noexcept;

/// Options declared by the heightfield importer.
[[nodiscard]] OptionsSchema heightfield_options() noexcept;

/// Imports `.r16`/`.raw` grids into a normalized u16 heightfield with explicit metre metadata.
class HeightfieldImporter final : public Importer {
public:
    [[nodiscard]] ImporterInfo info() const noexcept override;
    [[nodiscard]] OptionsSchema schema() const noexcept override;
    [[nodiscard]] Status import(const ImportRequest& request, ImportResult& out) noexcept override;
};

}  // namespace cy::import
