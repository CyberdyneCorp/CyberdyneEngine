// SPDX-License-Identifier: MIT
#pragma once
// Wire helpers for the editor services added after MaterialService: little-endian writers, a
// bounds-checked reader, and the schema-1 failure payload. Private to src/editor_backend.
//
// MaterialService keeps its own copies in its anonymous namespace. These were copied rather than
// moved, so the material and VFX paths stay byte-for-byte what #17 shipped.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/memory/array.h>

#include <cstring>
#include <string_view>

namespace cy::editor::wire {

[[nodiscard]] inline Status put_u8(Array<u8>& out, u8 value) noexcept {
    return out.push_back(value);
}

[[nodiscard]] inline Status put_u32(Array<u8>& out, u32 value) noexcept {
    for (u32 byte = 0; byte < 4; ++byte) {
        if (Status pushed = out.push_back(static_cast<u8>((value >> (byte * 8U)) & 0xFFU));
            !pushed) {
            return pushed;
        }
    }
    return ok();
}

[[nodiscard]] inline Status put_u64(Array<u8>& out, u64 value) noexcept {
    for (u32 byte = 0; byte < 8; ++byte) {
        if (Status pushed = out.push_back(static_cast<u8>((value >> (byte * 8U)) & 0xFFU));
            !pushed) {
            return pushed;
        }
    }
    return ok();
}

[[nodiscard]] inline Status put_i32(Array<u8>& out, i32 value) noexcept {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return put_u32(out, bits);
}

[[nodiscard]] inline Status put_f32(Array<u8>& out, f32 value) noexcept {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return put_u32(out, bits);
}

[[nodiscard]] inline Status put_vec3(Array<u8>& out, Vec3 value) noexcept {
    if (Status x = put_f32(out, value.x); !x) {
        return x;
    }
    if (Status y = put_f32(out, value.y); !y) {
        return y;
    }
    return put_f32(out, value.z);
}

[[nodiscard]] inline Status put_text(Array<u8>& out, std::string_view value) noexcept {
    if (Status length = put_u32(out, static_cast<u32>(value.size())); !length) {
        return length;
    }
    return out.append({reinterpret_cast<const u8*>(value.data()), value.size()});
}

/// Reads a request payload front to back. A read past the end sets `failed` and yields zero, so a
/// decoder reads every field and checks once at the end.
class Reader {
public:
    explicit Reader(Span<const u8> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] u8 read_u8() noexcept {
        if (!take(1)) {
            return 0;
        }
        return bytes_[cursor_ - 1];
    }

    [[nodiscard]] u32 read_u32() noexcept {
        if (!take(4)) {
            return 0;
        }
        u32 value = 0;
        for (u32 byte = 0; byte < 4; ++byte) {
            value |= static_cast<u32>(bytes_[cursor_ - 4 + byte]) << (byte * 8U);
        }
        return value;
    }

    [[nodiscard]] u64 read_u64() noexcept {
        const u64 low = read_u32();
        const u64 high = read_u32();
        return low | (high << 32U);
    }

    [[nodiscard]] f32 read_f32() noexcept {
        const u32 bits = read_u32();
        f32 value = 0.0F;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    [[nodiscard]] Vec3 read_vec3() noexcept {
        const f32 x = read_f32();
        const f32 y = read_f32();
        const f32 z = read_f32();
        return Vec3{x, y, z};
    }

    [[nodiscard]] Aabb read_aabb() noexcept {
        const Vec3 low = read_vec3();
        const Vec3 high = read_vec3();
        return Aabb::from_min_max(low, high);
    }

    /// A `u32` length and that many bytes. The view borrows the payload.
    [[nodiscard]] std::string_view read_text() noexcept {
        const u32 length = read_u32();
        if (!take(length)) {
            return {};
        }
        return {reinterpret_cast<const char*>(bytes_.data() + cursor_ - length), length};
    }

    /// True when every read so far was in bounds and nothing is left over.
    [[nodiscard]] bool complete() const noexcept { return !failed_ && cursor_ == bytes_.size(); }

private:
    [[nodiscard]] bool take(usize count) noexcept {
        if (failed_ || bytes_.size() - cursor_ < count) {
            failed_ = true;
            return false;
        }
        cursor_ += count;
        return true;
    }

    Span<const u8> bytes_;
    usize cursor_ = 0;
    bool failed_ = false;
};

/// Writes a payload and keeps the first failure, so an encoder checks once.
class Writer {
public:
    explicit Writer(Array<u8>& bytes) noexcept : bytes_(&bytes) {}

    Writer& u8v(u8 value) noexcept { return keep(put_u8(*bytes_, value)); }
    Writer& u32v(u32 value) noexcept { return keep(put_u32(*bytes_, value)); }
    Writer& u64v(u64 value) noexcept { return keep(put_u64(*bytes_, value)); }
    Writer& f32v(f32 value) noexcept { return keep(put_f32(*bytes_, value)); }
    Writer& f64v(f64 value) noexcept {
        u64 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return u64v(bits);
    }
    Writer& vec3(Vec3 value) noexcept { return keep(put_vec3(*bytes_, value)); }
    Writer& text(std::string_view value) noexcept { return keep(put_text(*bytes_, value)); }

    [[nodiscard]] Status status() const noexcept { return status_; }

private:
    Writer& keep(const Status& result) noexcept {
        if (status_ && !result) {
            status_ = result;
        }
        return *this;
    }

    Array<u8>* bytes_;
    Status status_ = ok();
};

/// The schema-1 failure payload every service shares: `u32 1, text code, text detail`.
[[nodiscard]] inline Status encode_failure(Array<u8>& out, std::string_view code,
                                           std::string_view detail) noexcept {
    out.clear();
    if (Status version = put_u32(out, 1); !version) {
        return version;
    }
    if (Status named = put_text(out, code); !named) {
        return named;
    }
    return put_text(out, detail);
}

}  // namespace cy::editor::wire
