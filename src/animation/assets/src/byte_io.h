// SPDX-License-Identifier: MIT
#pragma once
// Little-endian writing and bounds-checked reading for the cooked animation records. Private to
// src/animation/assets/src.
//
// The layout is the one `tools/import/` writes its skeleton and clip records in — u32 counts,
// IEEE floats by their bits, counted strings with no terminator — because a skeleton the importer
// cooked must read back here byte for byte.

#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>

#include <cstring>
#include <string_view>

namespace cy::animation::cooked {

class Writer {
public:
    explicit Writer(Array<u8>& out) noexcept : out_(&out) {}

    void u16_value(u16 value) noexcept {
        byte(static_cast<u8>(value & 0xFFU));
        byte(static_cast<u8>((value >> 8U) & 0xFFU));
    }
    void u32_value(u32 value) noexcept {
        for (u32 index = 0; index < 4; ++index) {
            byte(static_cast<u8>((value >> (index * 8U)) & 0xFFU));
        }
    }
    void u64_value(u64 value) noexcept {
        u32_value(static_cast<u32>(value & 0xFFFFFFFFULL));
        u32_value(static_cast<u32>(value >> 32U));
    }
    void f32_value(f32 value) noexcept {
        u32 bits = 0;
        static_assert(sizeof(bits) == sizeof(value));
        std::memcpy(&bits, &value, sizeof(bits));
        u32_value(bits);
    }
    /// A counted string: the length, then the bytes.
    void text(std::string_view value) noexcept {
        u32_value(static_cast<u32>(value.size()));
        for (const char character : value) {
            byte(static_cast<u8>(character));
        }
    }
    void name(Name value) noexcept { text(value.text()); }

    /// False once any append failed. Checked once at the end rather than after every field, which
    /// is the only way a record of a hundred fields reads as a record.
    [[nodiscard]] bool ok() const noexcept { return ok_; }

private:
    void byte(u8 value) noexcept { ok_ = ok_ && out_->push_back(value).has_value(); }

    Array<u8>* out_;
    bool ok_ = true;
};

/// A cursor over a payload that never reads past its end: a short read yields zeros and clears
/// `ok()`, so a decoder reads every field straight through and checks once.
class Reader {
public:
    explicit Reader(Span<const u8> payload) noexcept : payload_(payload) {}

    [[nodiscard]] u16 u16_value() noexcept {
        if (!take(2)) {
            return 0;
        }
        const u16 value = static_cast<u16>(payload_[cursor_] | (payload_[cursor_ + 1] << 8U));
        cursor_ += 2;
        return value;
    }
    [[nodiscard]] u32 u32_value() noexcept {
        if (!take(4)) {
            return 0;
        }
        u32 value = 0;
        for (u32 index = 0; index < 4; ++index) {
            value |= static_cast<u32>(payload_[cursor_ + index]) << (index * 8U);
        }
        cursor_ += 4;
        return value;
    }
    [[nodiscard]] u64 u64_value() noexcept {
        const u64 low = u32_value();
        const u64 high = u32_value();
        return low | (high << 32U);
    }
    [[nodiscard]] f32 f32_value() noexcept {
        const u32 bits = u32_value();
        f32 value = 0.0F;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    [[nodiscard]] std::string_view text() noexcept {
        const u32 length = u32_value();
        if (!take(length)) {
            return {};
        }
        const std::string_view value(reinterpret_cast<const char*>(payload_.data() + cursor_),
                                     length);
        cursor_ += length;
        return value;
    }
    [[nodiscard]] Name name() noexcept { return Name::intern(text()); }

    /// True when `count` records of at least `bytes` each could still fit: a count read from a
    /// corrupt payload is refused before anything is sized by it.
    [[nodiscard]] bool plausible(u64 count, u64 bytes) const noexcept {
        return count * bytes <= payload_.size() - cursor_;
    }
    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] bool finished() const noexcept { return ok_ && cursor_ == payload_.size(); }

private:
    [[nodiscard]] bool take(usize bytes) noexcept {
        if (!ok_ || bytes > payload_.size() - cursor_) {
            ok_ = false;
            return false;
        }
        return true;
    }

    Span<const u8> payload_;
    usize cursor_ = 0;
    bool ok_ = true;
};

}  // namespace cy::animation::cooked
