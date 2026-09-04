#ifndef CY_CORE_ASSETS_SRC_PACKAGE_FORMAT_H
#define CY_CORE_ASSETS_SRC_PACKAGE_FORMAT_H
// The byte-level shape of a `.cypak`, shared by the reader and the writer. Task 3.3.3.
//
// PRIVATE. Nothing outside this module knows the layout, which is what makes the format the
// engine's to change. Every field is little-endian and is written one byte at a time: a packed
// struct laid over a buffer is a compiler's opinion about padding, and the format is not.

#include <cy/core/base/types.h>

#include <cstring>

namespace cy::assets::format {

/// Fixed record sizes. Written out rather than derived from a struct, for the reason above.
inline constexpr usize kHeaderBytes = 112;
inline constexpr usize kEntryBytes = 96;
inline constexpr usize kChunkBytes = 72;
inline constexpr usize kFrameBytes = 16;
inline constexpr usize kDependencyBytes = 16;

/// The alignment a mappable chunk is placed on. A page on every platform the engine targets; the
/// reader checks the real page size at run time and reports a mismatch rather than assuming.
inline constexpr u64 kChunkAlignment = 4096;

// --- Little-endian scalars --------------------------------------------------------------------

inline void write_u8(u8* out, u8 value) noexcept {
    out[0] = value;
}

inline void write_u16(u8* out, u16 value) noexcept {
    out[0] = static_cast<u8>(value);
    out[1] = static_cast<u8>(value >> 8);
}

inline void write_u32(u8* out, u32 value) noexcept {
    for (usize i = 0; i < 4; ++i) {
        out[i] = static_cast<u8>(value >> (i * 8));
    }
}

inline void write_u64(u8* out, u64 value) noexcept {
    for (usize i = 0; i < 8; ++i) {
        out[i] = static_cast<u8>(value >> (i * 8));
    }
}

[[nodiscard]] inline u8 read_u8(const u8* in) noexcept {
    return in[0];
}

[[nodiscard]] inline u16 read_u16(const u8* in) noexcept {
    return static_cast<u16>(static_cast<u16>(in[0]) | (static_cast<u16>(in[1]) << 8));
}

[[nodiscard]] inline u32 read_u32(const u8* in) noexcept {
    u32 value = 0;
    for (usize i = 0; i < 4; ++i) {
        value |= static_cast<u32>(in[i]) << (i * 8);
    }
    return value;
}

[[nodiscard]] inline u64 read_u64(const u8* in) noexcept {
    u64 value = 0;
    for (usize i = 0; i < 8; ++i) {
        value |= static_cast<u64>(in[i]) << (i * 8);
    }
    return value;
}

/// Header field offsets, in one table so the reader and the writer cannot disagree about one.
namespace header {
inline constexpr usize kMagic = 0;              // 8 bytes
inline constexpr usize kFormatVersion = 8;      // u32
inline constexpr usize kFlags = 12;             // u32
inline constexpr usize kPayloadOffset = 16;     // u64
inline constexpr usize kDirectoryOffset = 24;   // u64
inline constexpr usize kDirectoryCount = 32;    // u32
inline constexpr usize kChunkOffset = 40;       // u64
inline constexpr usize kChunkCount = 48;        // u32
inline constexpr usize kFrameOffset = 56;       // u64
inline constexpr usize kFrameCount = 64;        // u32
inline constexpr usize kDependencyOffset = 72;  // u64
inline constexpr usize kDependencyCount = 80;   // u32
inline constexpr usize kManifestOffset = 88;    // u64
inline constexpr usize kManifestSize = 96;      // u32
inline constexpr usize kFileSize = 104;         // u64
}  // namespace header

}  // namespace cy::assets::format

#endif  // CY_CORE_ASSETS_SRC_PACKAGE_FORMAT_H
