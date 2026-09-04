#pragma once
// The debug draw API and the per-view debug modes. Task 4.1.6.
//
// `rendering-architecture` — "Debug visualisation": "A **debug draw** API SHALL allow any system or
// script to submit lines, spheres, boxes, capsules, frustums, and text, batched and drawn after the
// main scene", and the two scenarios are:
//
//   * "WHEN a gameplay system submits a debug sphere during simulation THEN it SHALL be double
//     buffered and drawn in the following frame with no allocation per primitive"
//   * "WHEN the engine is built for shipping THEN debug view modes and debug draw SHALL be compiled
//     out"
//
// --- HOW EACH OF THOSE IS ACTUALLY DELIVERED
// ------------------------------------------------------
//
// DOUBLE BUFFERED, DRAWN IN THE FOLLOWING FRAME. A system submits during the simulation half of the
// frame, which runs concurrently with the render half reading the other buffer. `swap()` at the
// frame boundary exchanges them. That is the same shape as `SnapshotBuffer` and for the same
// reason.
//
// NO ALLOCATION PER PRIMITIVE. Storage is reserved once at `initialize()` and never grows. A
// submission past the capacity is DROPPED and COUNTED — `dropped()` says how many — because growing
// would allocate inside a system body, and silently dropping without counting would make a missing
// debug sphere look like a bug in the thing being debugged.
//
// SUBMISSION IS SAFE FROM WORKER THREADS. Systems run on the job system, so "any system" means "any
// thread". A slot is claimed with one atomic increment and written to; there is no lock and nothing
// to contend on but the counter. THE ORDER PRIMITIVES END UP IN IS THEREFORE NOT DETERMINISTIC
// across threads, which is fine and is stated here rather than discovered: debug draw is
// presentation, it is excluded from golden images, and blending order among wireframe lines is not
// a visible property. Nothing else in the renderer is allowed to be ordered this way — see sort.h.
//
// COMPILED OUT. `kDebugVisualisationEnabled` (types.h) is false outside a development build, and
// every entry point below is `if constexpr`-guarded on it, so a shipping build reserves no storage
// and every `debug_draw.line(...)` call site compiles to nothing.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/servers/render/types.h>

#include <atomic>

namespace cy::render {

/// Every debug primitive is drawn as lines, so the renderer has one pipeline rather than six. The
/// tessellation of a sphere or a capsule into segments happens where the primitive is drawn, not
/// where it is submitted — a system submitting a sphere should not pay for thirty-two line segments
/// on the simulation thread.
enum class DebugShape : u8 {
    Line = 0,
    Sphere,
    Box,
    Capsule,
    Frustum,
    Count,
};

/// One submitted primitive. Fixed size, trivially copyable, and deliberately generous with the
/// fields so that six shapes share one struct rather than six arrays: at 64 bytes and a few
/// thousand primitives, the whole buffer fits comfortably and the renderer walks one array.
struct DebugPrimitive {
    DebugShape shape = DebugShape::Line;
    /// Drawn without depth testing, so it is visible through geometry. What a designer wants for a
    /// navigation query and not for a collision volume.
    bool depth_tested = true;
    /// Colour, linear, RGBA packed 8 bits per channel (`pack_color_unorm8`).
    u32 color = 0xFFFFFFFFU;
    /// Line: the two endpoints. Sphere: `a` is the centre. Box: `a` and `b` are the corners.
    /// Capsule: `a` and `b` are the segment endpoints. Frustum: `a` is the origin.
    Vec3 a{0.0F, 0.0F, 0.0F};
    Vec3 b{0.0F, 0.0F, 0.0F};
    /// Sphere and capsule radius; unused otherwise.
    f32 radius = 0.0F;
    /// Frustum: the view-projection whose corners are drawn. Only the frustum shape reads it, and
    /// it is here rather than in a variant because a variant would make the array non-trivial.
    Mat4 transform = Mat4::identity();
};

/// One submitted string, drawn in screen or world space.
struct DebugLabel {
    Vec3 position{0.0F, 0.0F, 0.0F};
    u32 color = 0xFFFFFFFFU;
    /// True when `position` is in pixels rather than world units.
    bool screen_space = false;
    /// Borrowed, and required to outlive the frame it is drawn in — a string literal, or storage
    /// the submitter owns for at least two frames. Copying would allocate per primitive, which is
    /// the one thing the requirement forbids.
    const char* text = "";
};

/// The double-buffered primitive store.
///
/// Not a server and not a singleton: a `RenderServer` owns one, and a test constructs its own.
class DebugDrawList {
public:
    explicit DebugDrawList(Allocator& allocator) noexcept;

    DebugDrawList(const DebugDrawList&) = delete;
    DebugDrawList& operator=(const DebugDrawList&) = delete;

    /// Reserve both buffers. In a build where debug visualisation is compiled out this reserves
    /// nothing and succeeds, so a host does not branch on the profile.
    [[nodiscard]] Status initialize(u32 primitive_capacity = 4096,
                                    u32 label_capacity = 256) noexcept;

    void line(Vec3 from, Vec3 to, u32 color, bool depth_tested = true) noexcept;
    void sphere(Vec3 center, f32 radius, u32 color, bool depth_tested = true) noexcept;
    void box(const Aabb& bounds, u32 color, bool depth_tested = true) noexcept;
    void capsule(Vec3 from, Vec3 to, f32 radius, u32 color, bool depth_tested = true) noexcept;
    void frustum(const Mat4& view_projection, u32 color, bool depth_tested = true) noexcept;
    void label(Vec3 position, const char* text, u32 color, bool screen_space = false) noexcept;

    /// Exchange the buffers. Called once per frame, on the frame thread, with no submission in
    /// flight — which is what "drawn in the following frame" means operationally.
    void swap() noexcept;

    /// The primitives to draw this frame: the buffer that was being written before the last swap.
    [[nodiscard]] Span<const DebugPrimitive> primitives() const noexcept;
    [[nodiscard]] Span<const DebugLabel> labels() const noexcept;

    /// How many submissions were dropped for want of capacity since `initialize()`. Non-zero means
    /// the capacity is too small, and it is reported rather than inferred from a missing sphere.
    [[nodiscard]] u32 dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

    [[nodiscard]] u32 capacity() const noexcept { return primitive_capacity_; }

private:
    struct Buffer {
        Array<DebugPrimitive> primitives;
        Array<DebugLabel> labels;
        std::atomic<u32> primitive_count{0};
        std::atomic<u32> label_count{0};

        explicit Buffer(Allocator& allocator) noexcept : primitives(allocator), labels(allocator) {}
    };

    void submit(const DebugPrimitive& primitive) noexcept;

    /// Initialised in the constructor because both elements need the allocator; the tidy check's
    /// suggested default member initializer would name a constructor parameter, which a default
    /// member initializer cannot see.
    // NOLINTNEXTLINE(modernize-use-default-member-init)
    Buffer buffers_[2];
    u32 writing_ = 0;
    u32 primitive_capacity_ = 0;
    u32 label_capacity_ = 0;
    std::atomic<u32> dropped_{0};
};

}  // namespace cy::render
