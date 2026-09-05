// The command frame's value identity. Task 4.1.4.

#include <cy/servers/input/frame.h>

#include <cstring>

namespace cy::input {
namespace {

/// FNV-1a over the frame's bytes. Chosen because it is short, has no dependency, and is stable
/// across compilers — this number appears in a desync report and two peers have to agree on it.
constexpr u64 kFnvOffset = 1469598103934665603ULL;
constexpr u64 kFnvPrime = 1099511628211ULL;

[[nodiscard]] u64 fold(u64 hash, const void* bytes, usize count) noexcept {
    const auto* data = static_cast<const u8*>(bytes);
    for (usize index = 0; index < count; ++index) {
        hash ^= data[index];
        hash *= kFnvPrime;
    }
    return hash;
}

}  // namespace

u64 CommandFrame::hash() const noexcept {
    // Field by field rather than over the whole struct: padding bytes are unspecified, and a hash
    // that included them would differ between two peers built by different compilers while the
    // input was identical — a desync report that names the wrong cause.
    u64 hash = kFnvOffset;
    hash = fold(hash, &tick, sizeof(tick));
    hash = fold(hash, &user, sizeof(user));
    hash = fold(hash, &pressed, sizeof(pressed));
    hash = fold(hash, &just_pressed, sizeof(just_pressed));
    hash = fold(hash, &just_released, sizeof(just_released));
    hash = fold(hash, &axis_count, sizeof(axis_count));
    for (u8 index = 0; index < axis_count && index < kMaxFrameAxes; ++index) {
        hash = fold(hash, &axes[index].x, sizeof(f32));
        hash = fold(hash, &axes[index].y, sizeof(f32));
    }
    return hash;
}

bool operator==(const CommandFrame& a, const CommandFrame& b) noexcept {
    if (a.tick != b.tick || a.user != b.user || a.pressed != b.pressed ||
        a.just_pressed != b.just_pressed || a.just_released != b.just_released ||
        a.axis_count != b.axis_count) {
        return false;
    }
    for (u8 index = 0; index < a.axis_count && index < kMaxFrameAxes; ++index) {
        if (a.axes[index].x != b.axes[index].x || a.axes[index].y != b.axes[index].y) {
            return false;
        }
    }
    return true;
}

}  // namespace cy::input
