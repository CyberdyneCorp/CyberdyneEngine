#pragma once
// SPDX-License-Identifier: MIT
// What `visbuffer.cpp` and `forward_visibility.cpp` share: the pass table, the binding table, the
// push block and the argument layout — each of them a mirror of `vg_visbuffer.slang`, and each of
// them a thing the two rasterisers must agree on. Private to this module's sources.

#include <cy/core/base/types.h>

namespace cy::rendering::vg::detail {

inline constexpr u32 kGroupSize = 64;

/// The pass order, and the index of each pipeline. `vgVisScan`, `vgVisPrepare` and
/// `vgVisHwPrepare` are one thread each; the rest are one thread per pixel or one workgroup per
/// visible cluster. The two `Hw` entries are the hardware rasteriser's compute halves — its draw is
/// a graphics pipeline `ForwardVisibility` owns, not a slot here.
enum VisPass : u32 {
    kPassClear = 0,
    kPassPrepare,
    kPassRaster,
    kPassUnpack,
    kPassClassify,
    kPassScan,
    kPassScatter,
    kPassResolve,
    kPassHwPrepare,
    kPassHwUnpack,
    kPassCount,
};

/// The visibility payload is `(identity << 8) | triangle`, settled with the depth key in one 64-bit
/// atomic. Both halves are bounded here and in `vg_visbuffer.slang`, and the two must move
/// together.
enum : u32 {
    kTrianglePayloadBits = 8U,
    kMaxTrianglesPacked = 1U << kTrianglePayloadBits,
};

enum VisBinding : u32 {
    kBindVisible = 0,
    kBindClusters,
    kBindGeometry,
    kBindInstances,
    kBindAssets,
    kBindPayload,
    kBindVisbuffer,
    kBindDepth,
    kBindBinCounts,
    kBindBinOffsets,
    kBindBinCursor,
    kBindBinPixels,
    kBindResolved,
    kBindCounters,
    kBindVisArgs,
    kBindHwPayload,
    kVisBindingCount,
};

/// The push block, matching `VgVisPush` in the shader field for field.
struct VisPush {
    f32 row0[4];
    f32 row1[4];
    f32 row2[4];
    f32 row3[4];
    u32 width;
    u32 height;
    u32 material_count;
    u32 vertex_stride;
    u32 normal_offset;
    u32 uv_offset;
    f32 position_scale;
    f32 normal_scale;
    f32 uv_scale;
    u32 cluster_stride;
    u32 draw_index_count;
    u32 visible_capacity;
};
static_assert(sizeof(VisPush) == 112, "VisPush must match VgVisPush in vg_visbuffer.slang");

/// `visArgs`: three words of raster dispatch, three of resolve dispatch, and five of the hardware
/// rasteriser's `VkDrawIndexedIndirectCommand`.
inline constexpr u64 kDrawArgsOffset = 6 * sizeof(u32);
inline constexpr u32 kDrawArgsStride = 5 * sizeof(u32);
inline constexpr u64 kVisArgsBytes = kDrawArgsOffset + kDrawArgsStride;

}  // namespace cy::rendering::vg::detail
