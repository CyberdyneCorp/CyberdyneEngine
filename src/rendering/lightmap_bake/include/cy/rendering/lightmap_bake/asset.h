// SPDX-License-Identifier: MIT
#pragma once
// The cooked lightmap: what a bake writes, what the build graph caches by content, and what a
// runtime uploads.
//
// `rendering-global-illumination` — "Lightmap baking". The payload is the texels as the device
// holds them — half floats, plane after plane, pages stacked — so loading one is a copy, and the
// per-instance addresses the level's draws carry as `gi_address`:
//
//     u32 magic 'CYLM'   u32 version    u32 mode        u32 page size
//     u32 pages          u32 gutter     u32 planes      u32 address count
//     u32 addresses[address count]
//     u16 texels[planes * page size * page size * pages * 4]
//     u32 shadow channels                                          version 2 onward
//     u64 shadow light ids[shadow channels], as two u32 low first
//     u16 shadow mask[page size * page size * pages * 4]           when shadow channels > 0
//
// Version 1 payloads — no shadow section — still decode, to a lightmap with no mask.
//
// Little-endian throughout, and nothing in it depends on the machine or the time it was baked, so a
// bake that is re-run over an unchanged level produces the same bytes — which is what lets the
// build graph's content key decide that it need not be re-run at all.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/lightmap_bake/bake.h>

namespace cy::rendering::lightmap_bake {

inline constexpr u32 kLightmapAssetMagic = 0x4D4C5943U;  // "CYLM"
inline constexpr u32 kLightmapAssetVersion = 2;
/// The oldest version `decode_lightmap_asset` still reads.
inline constexpr u32 kLightmapAssetOldestVersion = 1;

[[nodiscard]] Status encode_lightmap_asset(const BakedLightmap& lightmap, Array<u8>& out) noexcept;
/// Fails with `InvalidArgument` on a payload that is not a lightmap of this version or whose sizes
/// disagree with its length. The texels come back exactly as they were baked: the bake already
/// rounded them through half precision.
[[nodiscard]] Status decode_lightmap_asset(Span<const u8> payload, BakedLightmap& out) noexcept;

/// IEEE half precision, both ways. Round to nearest even.
[[nodiscard]] u16 half_from_float(f32 value) noexcept;
[[nodiscard]] f32 float_from_half(u16 half) noexcept;

}  // namespace cy::rendering::lightmap_bake
