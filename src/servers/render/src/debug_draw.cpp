// The debug draw store: lock-free submission, fixed capacity, double buffered. See
// cy/servers/render/debug_draw.h.

#include <cy/servers/render/debug_draw.h>

namespace cy::render {

DebugDrawList::DebugDrawList(Allocator& allocator) noexcept
    : buffers_{Buffer(allocator), Buffer(allocator)} {}

Status DebugDrawList::initialize(u32 primitive_capacity, u32 label_capacity) noexcept {
    if constexpr (!kDebugVisualisationEnabled) {
        // Compiled out: no storage, and every submit below returns before it touches anything.
        // Succeeding rather than failing is deliberate — a host that had to branch on the profile
        // to avoid an error would be a host with a shipping-only code path.
        return ok();
    } else {
        primitive_capacity_ = primitive_capacity;
        label_capacity_ = label_capacity;
        for (Buffer& buffer : buffers_) {
            // resize() rather than reserve(): a slot is claimed with an atomic increment and then
            // written by index, so the elements must already exist. This is the whole of "no
            // allocation per primitive" — the allocation happened here, once.
            if (Status sized = buffer.primitives.resize(primitive_capacity); !sized) {
                return sized;
            }
            if (Status sized = buffer.labels.resize(label_capacity); !sized) {
                return sized;
            }
            buffer.primitive_count.store(0, std::memory_order_relaxed);
            buffer.label_count.store(0, std::memory_order_relaxed);
        }
        return ok();
    }
}

void DebugDrawList::submit(const DebugPrimitive& primitive) noexcept {
    if constexpr (!kDebugVisualisationEnabled) {
        (void)primitive;
        return;
    } else {
        Buffer& buffer = buffers_[writing_];
        const u32 slot = buffer.primitive_count.fetch_add(1, std::memory_order_relaxed);
        if (slot >= primitive_capacity_) {
            // Put the counter back so a long-running frame does not run it away from the capacity;
            // the drop is counted instead.
            buffer.primitive_count.store(primitive_capacity_, std::memory_order_relaxed);
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        buffer.primitives[slot] = primitive;
    }
}

void DebugDrawList::line(Vec3 from, Vec3 to, u32 color, bool depth_tested) noexcept {
    DebugPrimitive primitive;
    primitive.shape = DebugShape::Line;
    primitive.depth_tested = depth_tested;
    primitive.color = color;
    primitive.a = from;
    primitive.b = to;
    submit(primitive);
}

void DebugDrawList::sphere(Vec3 center, f32 radius, u32 color, bool depth_tested) noexcept {
    DebugPrimitive primitive;
    primitive.shape = DebugShape::Sphere;
    primitive.depth_tested = depth_tested;
    primitive.color = color;
    primitive.a = center;
    primitive.radius = radius;
    submit(primitive);
}

void DebugDrawList::box(const Aabb& bounds, u32 color, bool depth_tested) noexcept {
    DebugPrimitive primitive;
    primitive.shape = DebugShape::Box;
    primitive.depth_tested = depth_tested;
    primitive.color = color;
    primitive.a = bounds.min;
    primitive.b = bounds.max;
    submit(primitive);
}

void DebugDrawList::capsule(Vec3 from, Vec3 to, f32 radius, u32 color, bool depth_tested) noexcept {
    DebugPrimitive primitive;
    primitive.shape = DebugShape::Capsule;
    primitive.depth_tested = depth_tested;
    primitive.color = color;
    primitive.a = from;
    primitive.b = to;
    primitive.radius = radius;
    submit(primitive);
}

void DebugDrawList::frustum(const Mat4& view_projection, u32 color, bool depth_tested) noexcept {
    DebugPrimitive primitive;
    primitive.shape = DebugShape::Frustum;
    primitive.depth_tested = depth_tested;
    primitive.color = color;
    primitive.transform = view_projection;
    submit(primitive);
}

void DebugDrawList::label(Vec3 position, const char* text, u32 color, bool screen_space) noexcept {
    if constexpr (!kDebugVisualisationEnabled) {
        (void)position;
        (void)text;
        (void)color;
        (void)screen_space;
        return;
    } else {
        Buffer& buffer = buffers_[writing_];
        const u32 slot = buffer.label_count.fetch_add(1, std::memory_order_relaxed);
        if (slot >= label_capacity_) {
            buffer.label_count.store(label_capacity_, std::memory_order_relaxed);
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        buffer.labels[slot] =
            DebugLabel{position, color, screen_space, (text != nullptr) ? text : ""};
    }
}

void DebugDrawList::swap() noexcept {
    if constexpr (!kDebugVisualisationEnabled) {
        return;
    } else {
        writing_ ^= 1U;
        buffers_[writing_].primitive_count.store(0, std::memory_order_relaxed);
        buffers_[writing_].label_count.store(0, std::memory_order_relaxed);
    }
}

Span<const DebugPrimitive> DebugDrawList::primitives() const noexcept {
    if constexpr (!kDebugVisualisationEnabled) {
        return {};
    } else {
        const Buffer& buffer = buffers_[writing_ ^ 1U];
        const u32 count = buffer.primitive_count.load(std::memory_order_relaxed);
        return {buffer.primitives.data(), count};
    }
}

Span<const DebugLabel> DebugDrawList::labels() const noexcept {
    if constexpr (!kDebugVisualisationEnabled) {
        return {};
    } else {
        const Buffer& buffer = buffers_[writing_ ^ 1U];
        const u32 count = buffer.label_count.load(std::memory_order_relaxed);
        return {buffer.labels.data(), count};
    }
}

}  // namespace cy::render
