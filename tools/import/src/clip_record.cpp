// The cooked clip payload's writer. See cy/import/clip_record.h for why it is not in fbx_clip.cpp.

#include <cy/import/clip_record.h>

#include <cy/import/fbx_clip.h>

#ifdef CY_IMPORT_ANIMATION
// The definition, which `cy/import/clip_record.h` only forward-declares — see the note there for
// why a public header may not include it.
#    include <cy/animation/clip.h>
#endif

#include <cstring>

#ifdef CY_IMPORT_ANIMATION

namespace cy::import {
namespace {

void put_u16(Array<u8>& out, u16 value) noexcept {
    (void)out.push_back(static_cast<u8>(value & 0xFFU));
    (void)out.push_back(static_cast<u8>((value >> 8U) & 0xFFU));
}

void put_u32(Array<u8>& out, u32 value) noexcept {
    for (u32 index = 0; index < 4; ++index) {
        (void)out.push_back(static_cast<u8>((value >> (index * 8U)) & 0xFFU));
    }
}

void put_f32(Array<u8>& out, f32 value) noexcept {
    u32 bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    put_u32(out, bits);
}

/// A counted string: the length, then the bytes. Never a terminator, because a name may not be
/// ASCII and a reader must not have to scan for an end the file did not promise.
void put_text(Array<u8>& out, std::string_view text) noexcept {
    put_u32(out, static_cast<u32>(text.size()));
    for (const char character : text) {
        (void)out.push_back(static_cast<u8>(character));
    }
}

}  // namespace

Status write_cooked_clip(const animation::Clip& clip, Span<const std::string_view> joint_names,
                         Array<u8>& out) noexcept {
    put_u32(out, kCookedClipVersion);
    put_text(out, clip.name().text());
    put_f32(out, clip.duration());
    put_u32(out, static_cast<u32>(clip.loop_mode()));
    put_f32(out, clip.sample_rate_hint());
    put_u32(out, clip.root_motion_joint());

    put_u32(out, static_cast<u32>(joint_names.size()));
    for (const std::string_view joint : joint_names) {
        put_text(out, joint);
    }

    put_u32(out, clip.track_count());
    for (const animation::TrackDesc& track : clip.tracks()) {
        put_u32(out, static_cast<u32>(track.kind));
        put_u32(out, static_cast<u32>(track.interpolation));
        put_u32(out, track.joint);
        put_u32(out, track.constant ? 1U : 0U);
        put_u32(out, track.first_key);
        put_u32(out, track.key_count);
        put_f32(out, track.range_min.x);
        put_f32(out, track.range_min.y);
        put_f32(out, track.range_min.z);
        put_f32(out, track.range_max.x);
        put_f32(out, track.range_max.y);
        put_f32(out, track.range_max.z);
    }

    put_u32(out, static_cast<u32>(clip.keys().size()));
    for (const animation::PackedKey& key : clip.keys()) {
        put_f32(out, key.time);
        for (const u16 component : key.c) {
            put_u16(out, component);
        }
    }
    return ok();
}

}  // namespace cy::import

#endif  // CY_IMPORT_ANIMATION
