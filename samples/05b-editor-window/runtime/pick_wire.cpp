// The editor's pick messages, decoded and answered. See pick_wire.h.

#include "pick_wire.h"

#include <cstring>

namespace cy::sample::editor_window {
namespace {

/// `cy_editor_core::codec`, reading. Little-endian, `f32` by its bits, bounds-checked at every step
/// because these bytes came off a socket.
class Reader {
public:
    explicit Reader(Span<const u8> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] bool ok() const noexcept { return !failed_; }

    [[nodiscard]] u8 u8_value() noexcept {
        if (failed_ || offset_ + 1 > bytes_.size()) {
            failed_ = true;
            return 0;
        }
        return bytes_[offset_++];
    }

    [[nodiscard]] u32 u32_value() noexcept { return static_cast<u32>(little_endian(4)); }
    [[nodiscard]] u64 u64_value() noexcept { return little_endian(8); }

    [[nodiscard]] f32 f32_value() noexcept {
        const u32 bits = u32_value();
        f32 value = 0.0F;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

private:
    [[nodiscard]] u64 little_endian(usize width) noexcept {
        if (failed_ || offset_ + width > bytes_.size()) {
            failed_ = true;
            return 0;
        }
        u64 value = 0;
        for (usize index = 0; index < width; ++index) {
            value |= static_cast<u64>(bytes_[offset_ + index]) << (index * 8U);
        }
        offset_ += width;
        return value;
    }

    Span<const u8> bytes_;
    usize offset_ = 0;
    bool failed_ = false;
};

/// A cap on how big a lasso may be before the request is refused rather than allocated for.
///
/// A malformed length prefix is the shape of failure a socket produces, and the honest response is
/// a refusal rather than a gigabyte. Ten thousand points is far more than a hand can draw.
constexpr u32 kMaxLassoPoints = 10'000;
/// The same, for the excluded list. A project with more than this many locked objects sends them
/// in a filter the engine cannot usefully apply anyway.
constexpr u32 kMaxExcluded = 100'000;

void write_u32(Array<u8>& out, u32 value, Status& status) noexcept {
    for (u32 index = 0; index < 4 && status; ++index) {
        status = out.push_back(static_cast<u8>(value >> (index * 8U)));
    }
}

void write_u64(Array<u8>& out, u64 value, Status& status) noexcept {
    for (u32 index = 0; index < 8 && status; ++index) {
        status = out.push_back(static_cast<u8>(value >> (index * 8U)));
    }
}

void write_f32(Array<u8>& out, f32 value, Status& status) noexcept {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    write_u32(out, bits, status);
}

}  // namespace

bool decode_pick_request(Span<const u8> bytes, PickRequest& out) noexcept {
    Reader reader(bytes);
    out.viewport = reader.u64_value();
    out.frame = reader.u64_value();
    const u8 kind = reader.u8_value();
    switch (static_cast<PickKind>(kind)) {
        case PickKind::Click:
            out.kind = PickKind::Click;
            out.x = reader.f32_value();
            out.y = reader.f32_value();
            break;
        case PickKind::Rectangle:
            out.kind = PickKind::Rectangle;
            out.min_x = reader.f32_value();
            out.min_y = reader.f32_value();
            out.max_x = reader.f32_value();
            out.max_y = reader.f32_value();
            break;
        case PickKind::Lasso: {
            out.kind = PickKind::Lasso;
            const u32 count = reader.u32_value();
            if (!reader.ok() || count > kMaxLassoPoints) {
                return false;
            }
            for (u32 index = 0; index < count; ++index) {
                const f32 x = reader.f32_value();
                const f32 y = reader.f32_value();
                if (!reader.ok() || !out.points.push_back(Vec2{x, y})) {
                    return false;
                }
            }
            break;
        }
        default:
            // An intent from a newer editor. Nothing after it can be located, so the request is
            // refused by name rather than answered against a misread filter.
            return false;
    }
    out.layers = reader.u32_value();
    out.include_transparent = reader.u8_value() != 0;
    out.max_candidates = reader.u32_value();
    const u32 excluded = reader.u32_value();
    if (!reader.ok() || excluded > kMaxExcluded) {
        return false;
    }
    for (u32 index = 0; index < excluded; ++index) {
        const u64 identity = reader.u64_value();
        if (!reader.ok() || !out.excluded.push_back(identity)) {
            return false;
        }
    }
    return reader.ok();
}

Status resolve_pick(const PickRequest& request, const render::View& view,
                    Span<const render::GpuInstance> instances, Span<const render::DrawItem> draws,
                    Array<render::PickCandidate>& out) noexcept {
    render::PickFilter filter;
    filter.include_transparent = request.include_transparent;
    filter.max_candidates = request.max_candidates;
    filter.excluded = request.excluded.span();
    switch (request.kind) {
        case PickKind::Click: {
            const Ray ray = render::ray_through_pixel(view, request.x, request.y);
            return render::pick_ray(instances, draws, ray, filter, out);
        }
        case PickKind::Rectangle: {
            const render::PickRect rect{request.min_x, request.min_y, request.max_x, request.max_y};
            return render::pick_rect(instances, view, draws, rect, filter, out);
        }
        case PickKind::Lasso:
            return render::pick_polygon(instances, view, draws, request.points.span(), filter, out);
    }
    return fail(ErrorCode::InvalidArgument, "a pick intent this build does not know");
}

Status encode_pick_response(u64 frame, Span<const render::PickCandidate> candidates,
                            Array<u8>& out) noexcept {
    Status status = ok();
    write_u64(out, frame, status);
    write_u32(out, static_cast<u32>(candidates.size()), status);
    for (const render::PickCandidate& candidate : candidates) {
        write_u64(out, candidate.stable_id, status);
        write_f32(out, candidate.distance, status);
        if (status) {
            status = out.push_back(static_cast<u8>(candidate.transparent ? 1 : 0));
        }
    }
    return status;
}

}  // namespace cy::sample::editor_window
