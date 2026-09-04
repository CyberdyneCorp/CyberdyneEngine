// ContentHash over the pinned blake3. Tasks 3.3.1 and 3.3.3.
//
// The only file in the engine that names blake3, which is what `thirdparty-dependencies` requires
// of an integrated dependency: its types appear nowhere else.

#include <cy/core/assets/hash.h>

#include <cy/core/base/assert.h>

#include <blake3.h>

#include <algorithm>
#include <cstring>

namespace cy::assets {
namespace {

// The opaque state in hash.h must be able to hold the real one. Checked here rather than trusted,
// because the failure mode of a too-small buffer is a stack overwrite that a test would not see.
constexpr usize kOpaqueStateBytes = 1936;
static_assert(sizeof(blake3_hasher) <= kOpaqueStateBytes,
              "ContentHasher::state_ is smaller than blake3_hasher; widen it to match");
static_assert(alignof(blake3_hasher) <= 64, "ContentHasher::state_ is under-aligned for blake3");

/// The opaque storage, as the implementation's own state.
///
/// Through `void*` ON PURPOSE, and the NOLINT is not a shrug. The direct reinterpret_cast the check
/// asks for is what -Wcast-align rejects — the engine builds with -Werror — because the source type
/// is `u8*`. `state_` is declared `alignas(64)`, which is checked against the real type by the
/// static_asserts above, so the alignment the warning is about is satisfied by construction.
blake3_hasher* as_hasher(u8* storage) noexcept {
    // NOLINTNEXTLINE(bugprone-casting-through-void)
    return static_cast<blake3_hasher*>(static_cast<void*>(storage));
}

constexpr char kHexDigits[] = "0123456789abcdef";

/// The value of one hex digit, or 255. Table-free: the ranges are three comparisons and no memory.
u8 hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return static_cast<u8>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<u8>(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return static_cast<u8>(c - 'A' + 10);
    }
    return 255;
}

}  // namespace

bool ContentHash::is_zero() const noexcept {
    return std::ranges::all_of(bytes, [](u8 byte) noexcept { return byte == 0; });
}

void ContentHash::format(char (&out)[kTextLength + 1]) const noexcept {
    for (usize i = 0; i < kByteLength; ++i) {
        out[i * 2] = kHexDigits[bytes[i] >> 4];
        out[(i * 2) + 1] = kHexDigits[bytes[i] & 0x0F];
    }
    out[kTextLength] = '\0';
}

Expected<ContentHash, Error> ContentHash::parse(std::string_view text) noexcept {
    if (text.size() != kTextLength) {
        return fail(ErrorCode::InvalidArgument, "a content hash is exactly 64 hex digits");
    }
    ContentHash out;
    for (usize i = 0; i < kByteLength; ++i) {
        const u8 high = hex_value(text[i * 2]);
        const u8 low = hex_value(text[(i * 2) + 1]);
        if (high == 255 || low == 255) {
            return fail(ErrorCode::InvalidArgument, "a content hash contains a non-hex character");
        }
        out.bytes[i] = static_cast<u8>((high << 4) | low);
    }
    return out;
}

bool operator==(const ContentHash& a, const ContentHash& b) noexcept {
    return std::memcmp(a.bytes, b.bytes, ContentHash::kByteLength) == 0;
}

bool operator<(const ContentHash& a, const ContentHash& b) noexcept {
    return std::memcmp(a.bytes, b.bytes, ContentHash::kByteLength) < 0;
}

ContentHash content_hash(const void* data, usize size) noexcept {
    ContentHasher hasher;
    hasher.update(data, size);
    return hasher.finish();
}

ContentHasher::ContentHasher() noexcept {
    blake3_hasher_init(as_hasher(state_));
}

void ContentHasher::update(const void* data, usize size) noexcept {
    CY_ASSERT_MSG(!finished_, "ContentHasher::update after finish(); call reset() first");
    if (size == 0) {
        return;
    }
    blake3_hasher_update(as_hasher(state_), data, size);
}

ContentHash ContentHasher::finish() noexcept {
    CY_ASSERT_MSG(!finished_, "ContentHasher::finish() twice");
    ContentHash out;
    blake3_hasher_finalize(as_hasher(state_), out.bytes, ContentHash::kByteLength);
    finished_ = true;
    return out;
}

void ContentHasher::reset() noexcept {
    blake3_hasher_init(as_hasher(state_));
    finished_ = false;
}

usize ContentHashHash::operator()(const ContentHash& hash) const noexcept {
    // The digest is already uniformly distributed, so the first machine word of it is the hash.
    u64 word = 0;
    std::memcpy(&word, hash.bytes, sizeof(word));
    return static_cast<usize>(word ^ (word >> 32));
}

}  // namespace cy::assets
