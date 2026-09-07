#include <cy/servers/render/culling/hzb.h>

#include <cy/core/math/matrix.h>

#include <cmath>
#include <new>

namespace cy::render::culling {
namespace {

[[nodiscard]] u32 halved(u32 value) noexcept {
    return value > 1U ? (value + 1U) / 2U : 1U;
}

[[nodiscard]] f32 clamp01(f32 value) noexcept {
    if (value < 0.0F) {
        return 0.0F;
    }
    return value > 1.0F ? 1.0F : value;
}

}  // namespace

u32 hzb_level_count(u32 width, u32 height) noexcept {
    if (width == 0 || height == 0) {
        return 0;
    }
    u32 levels = 1;
    while (width > 1U || height > 1U) {
        width = halved(width);
        height = halved(height);
        ++levels;
    }
    return levels;
}

ScreenRect project_sphere(Vec3 centre, f32 radius, const Mat4& view_projection, u32 width,
                          u32 height) noexcept {
    ScreenRect rect;
    if (width == 0 || height == 0 || !(radius > 0.0F)) {
        return rect;
    }

    // The projection of a sphere is an ellipse whose exact extent needs a quadratic. The bounding
    // box of the sphere is cheaper and CONTAINS that ellipse, which is the direction of error this
    // test is allowed to make: a rectangle larger than the silhouette can only fail to occlude.
    f32 min_x = 1.0F;
    f32 min_y = 1.0F;
    f32 max_x = 0.0F;
    f32 max_y = 0.0F;
    f32 nearest = 0.0F;
    for (u32 corner = 0; corner < 8; ++corner) {
        const Vec3 point{centre.x + ((corner & 1U) != 0U ? radius : -radius),
                         centre.y + ((corner & 2U) != 0U ? radius : -radius),
                         centre.z + ((corner & 4U) != 0U ? radius : -radius)};
        const Vec4 clip = view_projection * Vec4{point.x, point.y, point.z, 1.0F};
        if (!(clip.w > 1.0e-6F)) {
            // A corner behind or on the eye. The whole box straddles the near plane, so nothing
            // useful can be said about it and it must be drawn.
            return rect;
        }
        const f32 inverse_w = 1.0F / clip.w;
        const f32 ndc_x = clip.x * inverse_w;
        const f32 ndc_y = clip.y * inverse_w;
        const f32 depth = clip.z * inverse_w;
        const f32 screen_x = (ndc_x * 0.5F) + 0.5F;
        const f32 screen_y = (ndc_y * 0.5F) + 0.5F;
        min_x = screen_x < min_x ? screen_x : min_x;
        min_y = screen_y < min_y ? screen_y : min_y;
        max_x = screen_x > max_x ? screen_x : max_x;
        max_y = screen_y > max_y ? screen_y : max_y;
        // Reversed Z: the NEAREST point has the LARGEST depth.
        nearest = depth > nearest ? depth : nearest;
    }
    if (max_x <= 0.0F || max_y <= 0.0F || min_x >= 1.0F || min_y >= 1.0F) {
        return rect;
    }

    rect.min_x = clamp01(min_x) * static_cast<f32>(width);
    rect.min_y = clamp01(min_y) * static_cast<f32>(height);
    rect.max_x = clamp01(max_x) * static_cast<f32>(width);
    rect.max_y = clamp01(max_y) * static_cast<f32>(height);
    rect.nearest_depth = clamp01(nearest);
    rect.valid = true;
    return rect;
}

u32 hzb_level_for(const ScreenRect& rect, u32 level_count) noexcept {
    if (level_count == 0) {
        return 0;
    }
    const f32 span_x = rect.max_x - rect.min_x;
    const f32 span_y = rect.max_y - rect.min_y;
    const f32 span = span_x > span_y ? span_x : span_y;
    if (!(span > 1.0F)) {
        return 0;
    }
    // The level whose texels are at least half the rectangle across, so at most a 2x2 of them
    // covers it.
    const auto level = static_cast<u32>(std::ceil(std::log2(span)));
    return level < level_count ? level : level_count - 1U;
}

// --- Hzb ----------------------------------------------------------------------------------------

Hzb::Hzb(Allocator& allocator) noexcept : allocator_(&allocator), levels_(allocator) {}

Hzb::~Hzb() {
    release_levels();
}

/// `Array` is move-only and holds its own allocator, so growing an `Array<Array<f32>>` would move
/// the inner arrays and invalidate a span a caller already holds. The outer array therefore holds
/// pointers, which is the same arrangement `cy::rendering::CullWorkspace` reached for the same
/// reason — and it is why this class has a destructor at all.
void Hzb::release_levels() noexcept {
    for (Array<f32>* level : levels_) {
        level->~Array();
        allocator_->deallocate(level, sizeof(Array<f32>), alignof(Array<f32>));
    }
    levels_.clear();
}

Status Hzb::resize(u32 width, u32 height) noexcept {
    release_levels();
    width_ = width;
    height_ = height;
    level_count_ = hzb_level_count(width, height);
    valid_ = false;
    if (level_count_ == 0) {
        return ok();
    }
    if (Status reserved = levels_.reserve(level_count_); !reserved) {
        return reserved;
    }
    for (u32 level = 0; level < level_count_; ++level) {
        void* memory = allocator_->allocate(sizeof(Array<f32>), alignof(Array<f32>));
        if (memory == nullptr) {
            return fail(ErrorCode::OutOfMemory, "hierarchical depth buffer: level");
        }
        auto* depths = new (memory) Array<f32>(*allocator_);
        // Cleared to 0, which under reversed Z is the FAR plane: an unwritten pyramid occludes
        // nothing, which is the only safe initial state.
        const usize texels = static_cast<usize>(level_width(level)) * level_height(level);
        if (Status resized = depths->resize(texels); !resized) {
            depths->~Array();
            allocator_->deallocate(memory, sizeof(Array<f32>), alignof(Array<f32>));
            return resized;
        }
        for (usize index = 0; index < texels; ++index) {
            (*depths)[index] = 0.0F;
        }
        if (Status pushed = levels_.push_back(depths); !pushed) {
            depths->~Array();
            allocator_->deallocate(memory, sizeof(Array<f32>), alignof(Array<f32>));
            return pushed;
        }
    }
    return ok();
}

u32 Hzb::level_width(u32 level) const noexcept {
    u32 value = width_;
    for (u32 step = 0; step < level; ++step) {
        value = halved(value);
    }
    return value;
}

u32 Hzb::level_height(u32 level) const noexcept {
    u32 value = height_;
    for (u32 step = 0; step < level; ++step) {
        value = halved(value);
    }
    return value;
}

Span<f32> Hzb::level(u32 index) noexcept {
    if (index >= levels_.size()) {
        return {};
    }
    return levels_[index]->span();
}

Span<const f32> Hzb::level(u32 index) const noexcept {
    if (index >= levels_.size()) {
        return {};
    }
    return levels_[index]->span();
}

void Hzb::reduce() noexcept {
    for (u32 index = 1; index < level_count_; ++index) {
        const u32 parent_width = level_width(index);
        const u32 parent_height = level_height(index);
        const u32 child_width = level_width(index - 1U);
        const u32 child_height = level_height(index - 1U);
        const Span<const f32> child = level(index - 1U);
        const Span<f32> parent = level(index);
        for (u32 y = 0; y < parent_height; ++y) {
            for (u32 x = 0; x < parent_width; ++x) {
                // The FURTHEST depth of the footprint, which under reversed Z is the smallest. An
                // odd dimension folds the extra row and column into the last parent texel rather
                // than dropping it, which is what keeps the pyramid conservative at the edges.
                f32 furthest = 1.0F;
                const bool last_column =
                    (x * 2U) + 1U == (parent_width * 2U) - 1U && child_width % 2U == 1U;
                const bool last_row =
                    (y * 2U) + 1U == (parent_height * 2U) - 1U && child_height % 2U == 1U;
                const u32 last_x = last_column ? child_width - 1U : (x * 2U) + 1U;
                const u32 last_y = last_row ? child_height - 1U : (y * 2U) + 1U;
                for (u32 sample_y = y * 2U; sample_y <= last_y && sample_y < child_height;
                     ++sample_y) {
                    for (u32 sample_x = x * 2U; sample_x <= last_x && sample_x < child_width;
                         ++sample_x) {
                        const f32 value = child[(sample_y * child_width) + sample_x];
                        furthest = value < furthest ? value : furthest;
                    }
                }
                parent[(y * parent_width) + x] = furthest;
            }
        }
    }
}

bool Hzb::occludes(const ScreenRect& rect) const noexcept {
    if (!valid_ || !rect.valid || level_count_ == 0) {
        return false;
    }
    const u32 index = hzb_level_for(rect, level_count_);
    const u32 level_w = level_width(index);
    const u32 level_h = level_height(index);
    const f32 scale = 1.0F / static_cast<f32>(1U << index);
    auto to_texel = [](f32 value, u32 limit) noexcept -> u32 {
        if (!(value > 0.0F)) {
            return 0;
        }
        const auto texel = static_cast<u32>(value);
        return texel < limit ? texel : limit - 1U;
    };
    const u32 min_x = to_texel(rect.min_x * scale, level_w);
    const u32 max_x = to_texel(rect.max_x * scale, level_w);
    const u32 min_y = to_texel(rect.min_y * scale, level_h);
    const u32 max_y = to_texel(rect.max_y * scale, level_h);

    const Span<const f32> depths = level(index);
    if (depths.size() < static_cast<usize>(level_w) * level_h) {
        return false;
    }

    // The furthest depth anything already drawn reaches over the whole footprint.
    f32 furthest = 1.0F;
    for (u32 y = min_y; y <= max_y; ++y) {
        for (u32 x = min_x; x <= max_x; ++x) {
            const f32 value = depths[(static_cast<usize>(y) * level_w) + x];
            furthest = value < furthest ? value : furthest;
        }
    }
    // Occluded when the instance's NEAREST point is still further away than that — under reversed
    // Z, when its depth is smaller. Strictly less, so an instance exactly at the occluder's depth
    // is drawn: coplanar geometry is the case where a wrong answer is visible.
    return rect.nearest_depth < furthest;
}

// --- The tester ---------------------------------------------------------------------------------

HzbOcclusionTester::HzbOcclusionTester(const Hzb& pyramid, const Mat4& view_projection) noexcept
    : pyramid_(&pyramid), view_projection_(view_projection) {}

bool HzbOcclusionTester::occluded(Vec3 centre, f32 radius) const noexcept {
    const ScreenRect rect =
        project_sphere(centre, radius, view_projection_, pyramid_->width(), pyramid_->height());
    return pyramid_->occludes(rect);
}

// --- Hysteresis ---------------------------------------------------------------------------------

VisibilityHistory::VisibilityHistory(Allocator& allocator) noexcept : age_(allocator) {}

Status VisibilityHistory::resize(u32 slots) noexcept {
    if (Status resized = age_.resize(slots); !resized) {
        return resized;
    }
    clear();
    return ok();
}

void VisibilityHistory::mark_visible(u32 slot) noexcept {
    if (slot < age_.size()) {
        age_[slot] = 0;
    }
}

void VisibilityHistory::advance() noexcept {
    for (u8& age : age_) {
        if (age < 0xFFU) {
            ++age;
        }
    }
}

bool VisibilityHistory::recently_visible(u32 slot, u8 frames) const noexcept {
    if (frames == 0 || slot >= age_.size()) {
        return false;
    }
    return age_[slot] <= frames;
}

void VisibilityHistory::clear() noexcept {
    for (u8& age : age_) {
        age = 0xFFU;
    }
}

// --- The two-pass scheme ------------------------------------------------------------------------

TwoPassCull::TwoPassCull(Allocator& allocator) noexcept
    : previous_(allocator), current_(allocator) {}

Status TwoPassCull::resize(u32 slots) noexcept {
    if (Status resized = previous_.resize(slots); !resized) {
        return resized;
    }
    if (Status resized = current_.resize(slots); !resized) {
        return resized;
    }
    clear();
    return ok();
}

void TwoPassCull::record_visible(Span<const GpuDrawPayload> payloads) noexcept {
    for (const GpuDrawPayload& payload : payloads) {
        if (payload.instance_slot < current_.size()) {
            current_[payload.instance_slot] = 1U;
        }
    }
}

bool TwoPassCull::in_first_pass(u32 slot) const noexcept {
    return slot < previous_.size() && previous_[slot] != 0U;
}

void TwoPassCull::advance() noexcept {
    first_pass_count_ = 0;
    for (usize index = 0; index < previous_.size() && index < current_.size(); ++index) {
        previous_[index] = current_[index];
        first_pass_count_ += current_[index] != 0U ? 1U : 0U;
        current_[index] = 0U;
    }
}

void TwoPassCull::clear() noexcept {
    for (u8& slot : previous_) {
        slot = 0U;
    }
    for (u8& slot : current_) {
        slot = 0U;
    }
    first_pass_count_ = 0;
}

}  // namespace cy::render::culling
