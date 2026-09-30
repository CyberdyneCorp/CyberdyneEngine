// SPDX-License-Identifier: MIT
#pragma once
// The cooked irradiance volumes: what a lightmap bake captures beside the atlas.
//
// `rendering-global-illumination` — "Lightmap baking". A level's `volume` lines are captured by
// `capture_irradiance_volumes` with the bake's own tracer, and the probes are written as they came
// out of `gi::IrradianceVolume`, so a runtime that loads them configures a volume from the
// settings and holds exactly the probes the bake held:
//
//     u32 magic 'CYPV'   u32 version   u32 volume count
//     per volume:
//         u64 id, as two u32 low first
//         f32 origin[3]  f32 spacing   u32 count x   u32 count y   u32 count z   u32 rays
//         u32 probe count
//         per probe: f32 position[3], f32 payload[12], f32 axis distance[6], f32 validity
//
// Little-endian throughout, with each f32 as its bit pattern, and nothing in it depends on the
// machine or the time it was captured: an unchanged level captures to the same bytes.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/gi/irradiance_volume.h>

namespace cy::rendering::lightmap_bake {

inline constexpr u32 kProbeAssetMagic = 0x56505943U;  // "CYPV"
inline constexpr u32 kProbeAssetVersion = 1;

/// One captured volume to encode, and the caller's identity for it (the scene object's).
struct ProbeVolumeSource {
    u64 id = 0;
    const gi::IrradianceVolume* volume = nullptr;
};

/// One decoded volume: its settings and a run of `BakedProbes::probes`.
struct BakedProbeVolume {
    u64 id = 0;
    gi::IrradianceVolumeSettings settings{};
    u32 first_probe = 0;
    u32 probe_count = 0;
};

/// A decoded probe payload. Only the fields the payload carries are set on each probe: position,
/// payload, axis distances and validity; `captured` is true for every probe.
struct BakedProbes {
    Array<BakedProbeVolume> volumes;
    Array<gi::VolumeProbe> probes;
};

/// Fails with `InvalidArgument` on a null volume.
[[nodiscard]] Status encode_probe_asset(Span<const ProbeVolumeSource> volumes,
                                        Array<u8>& out) noexcept;
/// Fails with `InvalidArgument` on a payload that is not probes of this version or whose sizes
/// disagree with its length.
[[nodiscard]] Status decode_probe_asset(Span<const u8> payload, BakedProbes& out) noexcept;

}  // namespace cy::rendering::lightmap_bake
