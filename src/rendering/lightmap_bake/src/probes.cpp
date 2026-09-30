// SPDX-License-Identifier: MIT
// The cooked irradiance volumes. See probes.h.

#include <cy/rendering/lightmap_bake/probes.h>

#include <cstring>

namespace cy::rendering::lightmap_bake {
namespace {

/// Words one probe occupies: position, SH payload, axis distances, validity.
constexpr usize kProbeWords = 3U + 12U + 6U + 1U;
/// Words one volume's header occupies: id (2), origin (3), spacing, counts (3), rays, probe count.
constexpr usize kVolumeWords = 2U + 3U + 1U + 3U + 1U + 1U;

[[nodiscard]] Status put_u32(Array<u8>& out, u32 value) noexcept {
    for (u32 shift = 0; shift < 32U; shift += 8U) {
        if (Status pushed = out.push_back(static_cast<u8>((value >> shift) & 0xFFU)); !pushed) {
            return pushed;
        }
    }
    return ok();
}

[[nodiscard]] Status put_f32(Array<u8>& out, f32 value) noexcept {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return put_u32(out, bits);
}

[[nodiscard]] Status put_floats(Array<u8>& out, const f32* values, usize count) noexcept {
    for (usize index = 0; index < count; ++index) {
        if (Status put = put_f32(out, values[index]); !put) {
            return put;
        }
    }
    return ok();
}

/// A little-endian reader that refuses to run past the payload.
struct Reader {
    Span<const u8> bytes;
    usize at = 0;

    [[nodiscard]] bool has(usize words) const noexcept { return at + (words * 4U) <= bytes.size(); }
    [[nodiscard]] u32 u32_word() noexcept {
        const u8* word = bytes.data() + at;
        at += 4U;
        return u32{word[0]} | (u32{word[1]} << 8U) | (u32{word[2]} << 16U) | (u32{word[3]} << 24U);
    }
    [[nodiscard]] f32 f32_word() noexcept {
        const u32 bits = u32_word();
        f32 value = 0.0F;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    void floats(f32* out, usize count) noexcept {
        for (usize index = 0; index < count; ++index) {
            out[index] = f32_word();
        }
    }
};

[[nodiscard]] Status put_volume(Array<u8>& out, const ProbeVolumeSource& source) noexcept {
    if (source.volume == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a probe payload names a volume that is null");
    }
    const gi::IrradianceVolumeSettings& settings = source.volume->settings();
    const u32 words[] = {static_cast<u32>(source.id & 0xFFFFFFFFU),
                         static_cast<u32>(source.id >> 32U)};
    for (const u32 word : words) {
        if (Status put = put_u32(out, word); !put) {
            return put;
        }
    }
    const f32 origin[] = {settings.origin.x, settings.origin.y, settings.origin.z,
                          settings.spacing_metres};
    if (Status put = put_floats(out, origin, 4U); !put) {
        return put;
    }
    const u32 counts[] = {settings.count_x, settings.count_y, settings.count_z,
                          settings.rays_per_probe, source.volume->probe_count()};
    for (const u32 count : counts) {
        if (Status put = put_u32(out, count); !put) {
            return put;
        }
    }
    for (u32 index = 0; index < source.volume->probe_count(); ++index) {
        const gi::VolumeProbe& probe = source.volume->probe(index);
        const f32 position[] = {probe.position.x, probe.position.y, probe.position.z};
        if (Status put = put_floats(out, position, 3U); !put) {
            return put;
        }
        if (Status put = put_floats(out, probe.payload, 12U); !put) {
            return put;
        }
        if (Status put = put_floats(out, probe.axis_distance, 6U); !put) {
            return put;
        }
        if (Status put = put_f32(out, probe.validity); !put) {
            return put;
        }
    }
    return ok();
}

[[nodiscard]] Status get_volume(Reader& reader, BakedProbes& out) noexcept {
    if (!reader.has(kVolumeWords)) {
        return fail(ErrorCode::InvalidArgument, "a probe payload ends inside a volume");
    }
    BakedProbeVolume volume;
    const u32 low = reader.u32_word();
    volume.id = u64{low} | (u64{reader.u32_word()} << 32U);
    volume.settings.origin.x = reader.f32_word();
    volume.settings.origin.y = reader.f32_word();
    volume.settings.origin.z = reader.f32_word();
    volume.settings.spacing_metres = reader.f32_word();
    volume.settings.count_x = reader.u32_word();
    volume.settings.count_y = reader.u32_word();
    volume.settings.count_z = reader.u32_word();
    volume.settings.rays_per_probe = reader.u32_word();
    volume.probe_count = reader.u32_word();
    volume.first_probe = static_cast<u32>(out.probes.size());
    if (!reader.has(usize{volume.probe_count} * kProbeWords)) {
        return fail(ErrorCode::InvalidArgument, "a probe payload is shorter than its probes");
    }
    for (u32 index = 0; index < volume.probe_count; ++index) {
        gi::VolumeProbe probe;
        f32 position[3] = {};
        reader.floats(position, 3U);
        probe.position = Vec3{position[0], position[1], position[2]};
        reader.floats(probe.payload, 12U);
        reader.floats(probe.axis_distance, 6U);
        probe.validity = reader.f32_word();
        probe.captured = true;
        if (Status pushed = out.probes.push_back(probe); !pushed) {
            return pushed;
        }
    }
    return out.volumes.push_back(volume);
}

}  // namespace

Status encode_probe_asset(Span<const ProbeVolumeSource> volumes, Array<u8>& out) noexcept {
    out.clear();
    const u32 header[] = {kProbeAssetMagic, kProbeAssetVersion, static_cast<u32>(volumes.size())};
    for (const u32 word : header) {
        if (Status put = put_u32(out, word); !put) {
            return put;
        }
    }
    for (const ProbeVolumeSource& source : volumes) {
        if (Status put = put_volume(out, source); !put) {
            return put;
        }
    }
    return ok();
}

Status decode_probe_asset(Span<const u8> payload, BakedProbes& out) noexcept {
    out.volumes.clear();
    out.probes.clear();
    Reader reader{payload};
    if (!reader.has(3U) || reader.u32_word() != kProbeAssetMagic ||
        reader.u32_word() != kProbeAssetVersion) {
        return fail(ErrorCode::InvalidArgument, "not a version 1 probe payload");
    }
    const u32 volumes = reader.u32_word();
    for (u32 index = 0; index < volumes; ++index) {
        if (Status read = get_volume(reader, out); !read) {
            return read;
        }
    }
    if (reader.at != payload.size()) {
        return fail(ErrorCode::InvalidArgument, "a probe payload is longer than its volumes");
    }
    return ok();
}

}  // namespace cy::rendering::lightmap_bake
